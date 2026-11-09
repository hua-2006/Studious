import QtQuick
import QtQuick.Layouts
import QtQuick.Effects
import QtQuick.Controls
// 默认（Windows 原生）样式不允许改 background / contentItem，
// 显式用 Basic 样式覆盖同名类型，才能把输入控件做成模板里的 .input / .field-like。
import QtQuick.Controls.Basic
import Studious
// 配置页：按 docs/TempUI/index.html 的 config 段复刻。
//
// 结构（对应模板）：
//   页头            h1 + 副标题
//   引擎            模式切换（自动 / 强制）+ 一份四项列表：自动带序号且可拖排序，
//                   强制带单选圈（写 backend.engineId，与翻译页同一个属性）
//   引擎配置        一张卡 + 分段控件切四个面板，同一时刻只显示一个：
//                   Web 来源   单选的 provider（"" = 自动）
//                   云端 LLM   backend.llmConfig 的全部键
//                   本地 LLM   llama-server 的模型文件 / GPU / 启停
//                   本地 MT    语言对 → 保存位置
//
// 为什么引擎状态不再单独占一张卡：状态与引擎是一一对应的，拆成两处用户得来回对照；
// 直接做成行内副标签 + 行尾状态胶囊，点胶囊才跳到下面「引擎配置」卡里对应的 tab。
Item {
    id: page
    // 本地 MT 面板「下载 / 导入」→ 交给 Main.qml 切到设置页的资产分区（分类 mt）。
    // 本页自己不做路由，只发信号。
    signal openAssetsRequested(string kind)
    // ------------------------------------------------------------ 本地数据模型
    // 引擎的 canonical id。页面内部只认这四个，后端两套 id 的差异在边界上换算，
    // 免得 "llm" / "llm-cloud" 到处漏。mock 是开发占位，永远不进界面。
    readonly property var engineKeys: ["web", "llm", "llm-local", "local-mt"]
    // 自动引擎顺序：canonical id 数组，镜像 backend.enginePriority。
    // 拖动过程中只改这个数组（其余行实时让位），松手才写回后端 ——
    // 后端每写一次都要落 QSettings 并重探全部引擎，逐帧写会变成探测风暴。
    property var autoOrder: []
    // 拖动的临时状态：dragIndex 是抓起时的下标，dragTarget 是按手指位置算出的落点，
    // dragOffset 让被拖的那行跟手。
    property int dragIndex: -1
    property int dragTarget: -1
    property real dragOffset: 0
    // 行高固定：拖动换位全靠下标 × 行高算坐标，行高必须可预测，
    // 所以这里不交给布局自适应。
    readonly property int engineRowHeight: 56
    // 引擎卡的模式：false = 自动（按优先级降级），true = 强制某一个。
    // 只是界面状态，真正决定行为的是 backend.engineId / backend.enginePriority。
    property bool forceMode: false
    // 正在拖动（自动模式专用）：其余行要不要让位全靠它。
    readonly property bool autoModeDragging: dragIndex >= 0
    // 云端 LLM 的编辑副本。backend.llmConfig 是整张 map 覆盖写，
    // 改一个键也要先拷一份完整 map 再提交。
    property var llmDraft: ({})
    // 点胶囊跳转后要打开的那张卡的主下拉（空 = 只展开不开下拉）。
    property string pendingFocusKey: ""
    // 引擎配置卡片里当前显示的 tab：web / llm-cloud / llm-local / local-mt。
    property string configTab: "llm-cloud"
    onConfigTabChanged: {
        // Segmented 自己点的时候会直接写 current，把父级的绑定打断；从外面
        // 切 tab（点引擎行的状态胶囊、点「去配置」）时得手动把它拉回高亮位。
        if (configTabBar.current !== configTab) {
            configTabBar.current = configTab;
        }
        // 各面板的控件值（模型下拉、思考等级、本地模型文件）原来只在启动和
        // backend 信号里同步，切 tab 时不会刷新 —— 换引擎后看到的还是上一个引擎那份。
        syncLlmControls();
        syncLocalModelCombo();
    }
    // Web 来源：后端 listWebProviders() 是唯一真相，这里只补一条「自动」。
    readonly property var webProviderOptions: [
        {
            id: "",
            displayName: qsTr("自动"),
            note: qsTr("先试 Google，失败再退到 MyMemory；两个都不用 key。"),
            requiresApiKey: false
        }
    ].concat(backend.webProviders)
    // 云端模型：由「获取模型」调后端拉回来；当前配置里的模型名永远排第一项，
    // 保证下拉显示的是真实生效值。
    property var cloudModelOptions: []
    // 思考等级：下拉显示中文，落盘存协议里的英文码。
    readonly property var effortCodes: ["none", "minimal", "low", "medium", "high", ""]
    // 本地 LLM 的模型下拉：来自后端 LocalEngine::availableModels()。
    property var localModelOptions: []
    // 本地 MT 的可用路径不在这里存：后端现扫磁盘，QML 直接绑 backend.mtPairs。
    // 「获取模型」「探测」的反馈文案。后端回传的结论是英文（后端 i18n 固定英文），不译。
    readonly property string modelHint: backend.cloudBusy ? qsTr("正在向服务商请求…") : (backend.cloudModels.length > 0 ? qsTr("已拉取 %1 个聊天模型。").arg(backend.cloudModels.length) : "")
    readonly property string probeHint: backend.cloudProbeResult
    // 「浏览…」点了之后的实话（导入要走 manifest 校验，暂时没有按路径导入的接口）。
    property string importHint: ""
    // 表单下面那行说明字：以前每处手写一遍（左缩进 formLabelWidth + s3、tiny、inkVariant），
    // 现在只写一处 —— 用法 `Hint { text: qsTr("…") }`，需要置灰就再加 `opacity:`。
    // 深色主题下说明字用纯白：inkVariant 在深底上偏灰，再叠一层 0.45 的"不可用"透明
    // 就彻底看不清了（用户实测反馈）。
    component Hint: Text {
        Layout.fillWidth: true
        Layout.leftMargin: Theme.formLabelWidth + Theme.s3
        wrapMode: Text.WordWrap
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeTiny
        color: Theme.dark ? "#FFFFFF" : Theme.inkVariant
    }
    // ------------------------------------------------------------ id 换算
    // 后端有三套写法：engineId 的自动是空串、云端是 "llm-cloud"；
    // enginePriority / engineStatuses 里云端是 "llm"。全部折成 canonical。
    function canonicalId(id) {
        if (id === undefined || id === null || id === "" || id === "auto")
            return "auto";
        if (id === "llm" || id === "llm-cloud")
            return "llm";
        return id;
    }
    // canonical → backend.engineId 真正认的 code。从 backend.engines 里反查，
    // 后端哪天把 "llm-cloud" 改名，这里不用跟着改。
    function engineCode(key) {
        var list = backend.engines;
        for (var i = 0; i < list.length; ++i) {
            if (canonicalId(list[i].code) === key)
                return list[i].code;
        }
        return key;
    }
    // 引擎 canonical id → 显示名。
    // 不用 backend.engines 的 label：那套文案在后端 i18n 里，跟界面语言的切换时机不同步。
    function engineDisplayName(key) {
        // 名字只在后端 EngineIds.cpp 里定义一次（canonical 的 "llm" 折成云端 LLM）。
        return backend.engineName(key === "llm" ? "llm-cloud" : key);
    }
    // 在 engineStatuses 里查某个引擎的一条状态；查不到返回 null。
    function engineStatus(key) {
        var list = backend.engineStatuses;
        for (var i = 0; i < list.length; ++i) {
            if (canonicalId(list[i].id) === key)
                return list[i];
        }
        return null;
    }
    // 取不到就当未就绪：宁可让用户多点一次「去配置」，也不要把没探测到的引擎说成能用。
    // 口径统一在 Theme.engineStateOf（ready=false 压成 not-ready，别在这里再写一遍）。
    function engineState(key) {
        var row = engineStatus(key);
        return row ? Theme.engineStateOf(row) : "not-ready";
    }
    // 状态胶囊的文案。state 是资产级（装没装好），跟「当前语言对能不能用」是两回事。
    function engineStateLabel(key) {
        var state = page.engineState(key);
        if (state === "ready")
            return qsTr("就绪");
        if (state === "missing-assets")
            return qsTr("缺资产");
        return qsTr("未就绪");
    }
    // 状态点颜色。跟引擎卡行尾胶囊同一套语义：绿 = 就绪，黄 = 未就绪，红 = 缺资产/缺配置。
    // 强调色是用户自己的品牌色，拿它表达「能用 / 不能用」会读错，所以这里不走 Theme.primary。
    function tabBadgeColor(key) {
        return Theme.engineStateColor(page.engineState(key));
    }
    // 引擎配置卡分段控件的状态点：tab code → 颜色。
    // tab code 与 engineStatuses 的 canonical id 只差云端那一项（tab 叫 llm-cloud，引擎叫 llm）。
    readonly property var configTabBadges: ({
            "web": page.tabBadgeColor("web"),
            "llm-cloud": page.tabBadgeColor("llm"),
            "llm-local": page.tabBadgeColor("llm-local"),
            "local-mt": page.tabBadgeColor("local-mt")
        })
    // 当前源语言 / 目标语言下能不能真的解析出翻译路径 —— 后端新给的 ready 字段。
    // 不能拿 engineState() === "ready" 代替：本地 MT 装好了但源语言是「自动检测」时，
    // state 仍是 ready，ready 却是 false；自动模式拿 state 会被它错高亮成「实际会用的引擎」。
    function engineResolvable(key) {
        var row = page.engineStatus(key);
        return !!(row && row.ready);
    }
    // 行图标：跟左侧导航用同一批 SVG，按主题重新上色。
    function engineIcon(key) {
        if (key === "web")
            return "qrc:/qt/qml/Studious/resources/icons/globe.svg";
        if (key === "llm")
            return "qrc:/qt/qml/Studious/resources/icons/translate.svg";
        if (key === "llm-local")
            return "qrc:/qt/qml/Studious/resources/icons/settings.svg";
        if (key === "local-mt")
            return "qrc:/qt/qml/Studious/resources/icons/database.svg";
        return "";
    }
    // 副标签：只显示后端给的 detail（中文，原样显示、不翻译）。
    // 状态已经独立成行尾的胶囊，这里不再拼「未就绪 · 」之类的前缀。
    function engineSubtitle(key) {
        var row = page.engineStatus(key);
        return (row && row.detail !== undefined && row.detail !== null) ? String(row.detail) : "";
    }
    // 自动模式下当前真正会被用到的那个：autoOrder 里第一个「能解析出路径」的。
    // 一个都没有就返回空 —— 不高亮任何行，别装作有引擎能用。
    readonly property string highlightEngineKey: {
        if (page.forceMode)
            return "";
        for (var i = 0; i < page.autoOrder.length; ++i) {
            if (page.engineResolvable(page.autoOrder[i]))
                return page.autoOrder[i];
        }
        return "";
    }
    // 切模式。切回自动要顺手把 engineId 归零（同一个属性，翻译页跟着回自动）；
    // 切到强制则保持现状，没选过任何引擎时四个单选都不亮，由右上角提示说明原因。
    function applyForceMode(force) {
        page.forceMode = force;
        var code = force ? "force" : "auto";
        if (engineModeSeg.current !== code)
            engineModeSeg.current = code;
    }
    function setEngineMode(force) {
        page.applyForceMode(force);
        if (!force)
            backend.engineId = page.engineCode("auto");
    }
    // 本地服务「此刻是否在跑」。注意不能再用 engineState("llm-local")：那条现在报的是
    // 资产级可用性（运行时 + 模型在不在），服务没跑时也是 ready。跑没跑只看 PID。
    readonly property bool localLlmReady: backend.localPid > 0
    // 本地 LLM 能不能真的启动：运行时（llama-server.exe）+ 至少一个模型文件。
    // 缺任何一样，「启动」按钮就禁用 —— 让用户去「资产」页补齐，而不是让他点一下再看日志。
    readonly property bool localAssetsReady: backend.localGpuAvailable || backend.localCpuAvailable
    readonly property bool localHasModel: backend.localModels.length > 0
    readonly property bool localCanStart: page.localAssetsReady && page.localHasModel
    // 手动引擎的选中判断。backend.engineId 可能是自动，这时四项都不选中。
    function engineSelected(key) {
        return canonicalId(backend.engineId) === key;
    }
    // ------------------------------------------------------------ 顺序同步
    function syncAutoOrder() {
        var stored = backend.enginePriority || [];
        var next = [];
        for (var i = 0; i < stored.length; ++i) {
            var key = canonicalId(stored[i]);
            if (page.engineKeys.indexOf(key) >= 0 && next.indexOf(key) < 0)
                next.push(key);
        }
        // 后端可能还没落过盘（空）或缺项；补齐，保证自动引擎永远是四项。
        for (var j = 0; j < page.engineKeys.length; ++j) {
            if (next.indexOf(page.engineKeys[j]) < 0)
                next.push(page.engineKeys[j]);
        }
        page.autoOrder = next;
    }
    function beginDrag(index) {
        page.dragIndex = index;
        page.dragTarget = index;
        page.dragOffset = 0;
        // 拖动期间关掉 Flickable 的惯性拖动，否则竖着拖手柄会把整页一起滚。
        if (scroll.contentItem)
            scroll.contentItem.interactive = false;
    }
    function updateDrag(translationY) {
        if (page.dragIndex < 0)
            return;
        page.dragOffset = translationY;
        var rowH = page.engineRowHeight;
        var center = page.dragIndex * rowH + page.dragOffset + rowH / 2;
        var slot = Math.floor(center / rowH);
        page.dragTarget = Math.max(0, Math.min(slot, page.autoOrder.length - 1));
    }
    function commitDrag() {
        var from = page.dragIndex;
        var to = page.dragTarget;
        page.dragIndex = -1;
        page.dragTarget = -1;
        page.dragOffset = 0;
        if (scroll.contentItem)
            scroll.contentItem.interactive = true;
        if (from < 0 || to < 0 || from === to)
            return;
        var next = page.autoOrder.slice();
        var moved = next.splice(from, 1)[0];
        next.splice(to, 0, moved);
        // 延到下一拍再落地：这里正在 delegate 自己的信号处理里跑，
        // 立刻换 model 会把当前 delegate 连同 DragHandler 一起销毁。
        Qt.callLater(function () {
            page.autoOrder = next;
            // 写回即落盘，后端会顺手重算「自动」的解析结果。
            backend.enginePriority = next;
        });
    }
    // ------------------------------------------------------------ 云端 LLM 读写
    // 后端整张 map 覆盖写，所以每次都要提交完整对象，不能只给一个键。
    function patchLlm(key, value) {
        var next = Object.assign({}, page.llmDraft);
        next[key] = value;
        page.llmDraft = next;
        backend.llmConfig = next;
    }
    function llmString(key) {
        var v = page.llmDraft ? page.llmDraft[key] : "";
        return (v === undefined || v === null) ? "" : String(v);
    }
    function llmNumber(key, fallback) {
        var n = Number(page.llmDraft ? page.llmDraft[key] : NaN);
        return isNaN(n) ? fallback : n;
    }
    function llmBool(key) {
        return page.llmDraft && page.llmDraft[key] === true;
    }
    // 当前配置里的模型名永远排第一，其次是网关拉回来的模型列表。
    function rebuildCloudModelOptions() {
        var opts = [];
        var current = page.llmString("model");
        if (current.length > 0)
            opts.push(current);
        var list = backend.cloudModels;
        for (var i = 0; i < list.length; ++i) {
            if (opts.indexOf(list[i].id) < 0)
                opts.push(list[i].id);
        }
        page.cloudModelOptions = opts;
    }
    function cloudModelIndex() {
        var idx = page.cloudModelOptions.indexOf(page.llmString("model"));
        return idx < 0 ? 0 : idx;
    }
    function effortIndex(code) {
        var idx = page.effortCodes.indexOf(code);
        return idx < 0 ? 0 : idx;
    }
    // 下拉的 currentIndex 一旦被用户点过就不再跟着绑定走，
    // 所以外部（后端）变了之后要手动对一次。
    function syncLlmControls() {
        var modelIdx = page.cloudModelIndex();
        if (cloudModelCombo.currentIndex !== modelIdx)
            cloudModelCombo.currentIndex = modelIdx;
        var eIdx = page.effortIndex(page.llmString("reasoningEffort"));
        if (effortCombo.currentIndex !== eIdx)
            effortCombo.currentIndex = eIdx;
    }
    // ------------------------------------------------------------ 本地 LLM 读写
    function refreshLocalModelOptions() {
        var opts = [qsTr("自动（第一个可用）")];
        var list = backend.localModels;
        for (var i = 0; i < list.length; ++i)
            opts.push(list[i]);
        // 正在用的模型即使不在清单里也要能显示出来，否则下拉会说谎。
        if (backend.localModel.length > 0 && opts.indexOf(backend.localModel) < 0)
            opts.push(backend.localModel);
        page.localModelOptions = opts;
    }
    function syncLocalModelCombo() {
        page.refreshLocalModelOptions();
        var idx = backend.localModel.length > 0 ? page.localModelOptions.indexOf(backend.localModel) : 0;
        if (idx < 0)
            idx = 0;
        if (localModelCombo.currentIndex !== idx)
            localModelCombo.currentIndex = idx;
    }
    // ------------------------------------------------------------ 跳转
    // 引擎 id → 引擎配置卡里的 tab code。自动 / 空返回空串（停在原来的 tab 不动）。
    function configTabFor(key) {
        const id = page.canonicalId(key);
        if (id === "web" || id === "llm" || id === "llm-local" || id === "local-mt")
            return id === "llm" ? "llm-cloud" : id;
        return "";
    }
    // 四张配置卡合并成一张之后，认得的引擎一律指向同一个 detailCard（真正换的是 tab）。
    function cardForEngine(key) {
        if (key === "web" || key === "llm" || key === "llm-cloud" || key === "llm-local" || key === "local-mt")
            return detailCard;
        return null;
    }
    function scrollToCard(card, animated) {
        var flick = scroll.contentItem;
        if (!flick || !card)
            return;
        var maxY = Math.max(0, flick.contentHeight - flick.height);
        var target = Math.max(0, Math.min(card.y - Theme.s4, maxY));
        scrollAnim.stop();
        if (animated === false) {
            flick.contentY = target;
            return;
        }
        scrollAnim.from = flick.contentY;
        scrollAnim.to = target;
        scrollAnim.start();
    }
    // guard = true 表示「带拦截的跳转」：当前语言对下真用不了的引擎先弹框拦一下，
    // 别把人丢进一张填了一半的表。guard = false 表示「点状态胶囊」或框里的「去配置」：
    // 已经拦过一次或有明确意图了，直接跳。
    // 现状：点解析不了的行胶囊走 notReadyDialog.openFor()（不经过本函数）；
    // 本函数只留 guard 参数给「框里点去配置」与「解析得了的行胶囊」用。
    function openEngineConfig(key, guard) {
        var card = page.cardForEngine(key);
        if (!card)
            return;
        if (guard && !page.engineResolvable(key)) {
            notReadyDialog.openFor(key);
            return;
        }
        // 先切 tab 再展开 / 滚动：面板一换内容高度就变了，顺序反了 scrollToCard 算出来的位置会偏。
        const tab = page.configTabFor(key);
        if (tab !== "")
            page.configTab = tab;
        card.expanded = true;
        page.pendingFocusKey = key;
        page.scrollToCard(card, true);
        // 卡片高度有 250ms 动画，内容高度会变；等布局落定再补正一次并开下拉。
        focusTimer.restart();
    }
    Component.onCompleted: {
        page.syncAutoOrder();
        // 上次落盘的是「强制某个引擎」就按强制打开，否则按自动。
        page.applyForceMode(page.canonicalId(backend.engineId) !== "auto");
        // 强制模式下直接把配置卡切到那个引擎；自动模式留在默认的 llm-cloud。
        const initialTab = page.configTabFor(backend.engineId);
        if (initialTab !== "")
            page.configTab = initialTab;
        page.llmDraft = backend.llmConfig;
        page.rebuildCloudModelOptions();
        page.refreshLocalModelOptions();
        backend.refreshEngineStatuses();
    }
    Connections {
        target: backend
        function onEnginePriorityChanged() {
            page.syncAutoOrder();
        }
        // 翻译页改了引擎选择，这里要跟着换模式，否则界面会撒谎。
        function onEngineIdChanged() {
            page.applyForceMode(page.canonicalId(backend.engineId) !== "auto");
            // 配置卡也顺手切到同一个引擎的 tab（自动模式不切，免得把用户当前看的 tab 抢走）。
            const tab = page.configTabFor(backend.engineId);
            if (tab !== "")
                page.configTab = tab;
        }
        function onLlmConfigChanged() {
            page.llmDraft = backend.llmConfig;
            page.rebuildCloudModelOptions();
            page.syncLlmControls();
        }
        function onLocalModelChanged() {
            page.syncLocalModelCombo();
        }
        function onCloudModelsChanged() {
            page.rebuildCloudModelOptions();
            page.syncLlmControls();
        }
    }
    // 跳转后的收尾：定位到最终位置，需要的话把主下拉拉下来。
    Timer {
        id: focusTimer
        interval: Theme.durBase + 80
        onTriggered: {
            var key = page.pendingFocusKey;
            page.pendingFocusKey = "";
            if (key === "")
                return;
            page.scrollToCard(page.cardForEngine(key), false);
            // ComboBox 自己没有 open()，得直接开自定义的 popup。
            if (key === "llm")
                cloudModelCombo.popup.open();
            else if (key === "llm-local")
                localModelCombo.popup.open();
        }
    }
    NumberAnimation {
        id: scrollAnim
        target: scroll.contentItem
        property: "contentY"
        duration: Theme.durBase
        easing.type: Easing.OutCubic
    }
    // -------------------------------------------------------- 内联样式组件
    // 输入框：模板里的 .input。
    component ThemedField: TextField {
        id: field
        Layout.fillWidth: true
        implicitHeight: 38
        color: Theme.ink
        placeholderTextColor: Theme.inkVariant
        selectionColor: Theme.primaryContainer
        selectedTextColor: Theme.primaryContainerText
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        leftPadding: Theme.s3
        rightPadding: Theme.s3
        background: Rectangle {
            radius: Theme.rMd
            color: field.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
            border.width: field.activeFocus ? 1 : 0
            border.color: Theme.primary
            Behavior on color {
                ColorAnimation {
                    duration: Theme.durFast
                }
            }
        }
    }
    // 下拉：模板里的 .field-like + .menu。
    component ThemedCombo: ComboBox {
        id: combo
        Layout.fillWidth: true
        implicitHeight: 38
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        leftPadding: Theme.s3
        rightPadding: Theme.s3
        topPadding: 0
        bottomPadding: 0
        background: Rectangle {
            radius: Theme.rMd
            color: combo.hovered ? Theme.surfaceHigh : Theme.surfaceHighest
            border.width: 0
            Behavior on color {
                ColorAnimation {
                    duration: Theme.durFast
                }
            }
        }
        contentItem: Text {
            rightPadding: combo.indicator.width + Theme.s2
            text: combo.displayText
            font: combo.font
            color: Theme.ink
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }
        indicator: Text {
            x: combo.width - width - combo.rightPadding
            y: combo.topPadding + (combo.availableHeight - height) / 2
            text: "\u25BE"
            font.family: Theme.fontUi
            font.pixelSize: Theme.fontSizeBody
            color: Theme.inkVariant
        }
        delegate: ItemDelegate {
            id: comboItem
            width: combo.width - Theme.s3
            height: 40
            required property int index
            required property var modelData
            text: comboItem.modelData !== undefined ? String(comboItem.modelData) : ""
            highlighted: combo.highlightedIndex === index
            contentItem: Text {
                text: comboItem.text
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: Theme.ink
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
            }
            background: Rectangle {
                radius: Theme.rSm
                color: comboItem.highlighted ? Theme.surfaceHighest : "transparent"
            }
        }
        popup: Popup {
            y: combo.height + Theme.s1
            width: combo.width
            implicitHeight: Math.min(contentItem.implicitHeight + Theme.s2, 320)
            padding: Theme.s1
            background: Rectangle {
                radius: Theme.rMd
                color: Theme.surfaceHigh
                border.width: 1
                border.color: Theme.outlineVariant
            }
            contentItem: ListView {
                clip: true
                implicitHeight: contentHeight
                model: combo.popup.visible ? combo.delegateModel : null
                currentIndex: combo.highlightedIndex
                boundsBehavior: Flickable.StopAtBounds
                ScrollIndicator.vertical: ScrollIndicator {}
            }
        }
    }
    // 数字框：Basic 样式默认把 +/- 画成两块整高色块，这里换成右侧两个细按钮。
    component ThemedSpin: SpinBox {
        id: spin
        readonly property int stepWidth: 26
        Layout.fillWidth: true
        implicitHeight: 38
        editable: true
        font.family: Theme.fontUi
        font.pixelSize: Theme.fontSizeBody
        leftPadding: Theme.s3
        rightPadding: spin.stepWidth * 2 + Theme.s1
        background: Rectangle {
            radius: Theme.rMd
            color: Theme.surfaceHighest
            border.width: spin.activeFocus ? 1 : 0
            border.color: Theme.primary
        }
        contentItem: TextInput {
            text: spin.displayText
            font: spin.font
            color: Theme.ink
            selectionColor: Theme.primaryContainer
            selectedTextColor: Theme.primaryContainerText
            horizontalAlignment: Qt.AlignLeft
            verticalAlignment: Text.AlignVCenter
            readOnly: !spin.editable
            validator: spin.validator
            inputMethodHints: spin.inputMethodHints
        }
        up.indicator: Rectangle {
            x: spin.width - width
            height: spin.height
            implicitWidth: spin.stepWidth
            radius: Theme.rSm
            color: spin.up.hovered ? Theme.surfaceHigh : "transparent"
            Text {
                anchors.centerIn: parent
                text: "\uFF0B"
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: spin.up.pressed ? Theme.ink : Theme.inkVariant
            }
        }
        down.indicator: Rectangle {
            x: spin.width - spin.stepWidth * 2
            height: spin.height
            implicitWidth: spin.stepWidth
            radius: Theme.rSm
            color: spin.down.hovered ? Theme.surfaceHigh : "transparent"
            Text {
                anchors.centerIn: parent
                text: "\uFF0D"
                font.family: Theme.fontUi
                font.pixelSize: Theme.fontSizeBody
                color: spin.down.pressed ? Theme.ink : Theme.inkVariant
            }
        }
    }
    // 页内折叠卡片：外壳样式与 SectionCard 完全一致，只是内容可收起。
    // 头部一行（标题 + hint + 可选右侧操作 + ▾/▴），整行可点；收起后只留头部高度。
    component CollapsibleCard: Rectangle {
        id: acc
        property string title: ""
        property string hint: ""
        property bool expanded: false
        // false = 常态展开、不画折叠箭头、点头部也不收起。
        // 引擎卡与引擎配置卡用这个：里面是分段控件，折叠起来只会挡住状态。
        property bool collapsible: true
        default property alias content: body.data
        property alias headerActions: actions.data
        signal toggled(bool expanded)
        implicitHeight: column.implicitHeight + Theme.s5 * 2
        radius: Theme.rLg
        color: Theme.surfaceLow
        border.width: 1
        border.color: Theme.outlineVariant
        // 收起动画期间内容会先溢出，靠 clip 裁掉。
        clip: true
        Behavior on height {
            NumberAnimation {
                duration: Theme.durBase
                easing.type: Easing.OutCubic
            }
        }
        ColumnLayout {
            id: column
            anchors.fill: parent
            anchors.margins: Theme.s5
            spacing: Theme.s3
            // 头部整行：点哪里都切换折叠。MouseArea 在下层，
            // 头部里的按钮在上层，所以「刷新」这类操作不会被折叠抢走。
            Item {
                Layout.fillWidth: true
                implicitHeight: headRow.implicitHeight
                MouseArea {
                    anchors.fill: parent
                    enabled: acc.collapsible
                    cursorShape: acc.collapsible ? Qt.PointingHandCursor : Qt.ArrowCursor
                    onClicked: {
                        acc.expanded = !acc.expanded;
                        acc.toggled(acc.expanded);
                    }
                }
                RowLayout {
                    id: headRow
                    anchors.fill: parent
                    spacing: Theme.s2
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: acc.title
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSection
                        font.weight: Font.DemiBold
                        color: Theme.ink
                    }
                    Text {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        visible: text.length > 0
                        text: acc.hint
                        elide: Text.ElideRight
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.inkVariant
                    }
                    Row {
                        id: actions
                        Layout.alignment: Qt.AlignVCenter
                        spacing: Theme.s2
                    }
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        visible: acc.collapsible
                        text: acc.expanded ? "\u25B4" : "\u25BE"
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeBody
                        color: Theme.inkVariant
                    }
                }
            }
            // 内容区：收起时整块隐藏，ColumnLayout 连带把它的间距一起让出来。
            ColumnLayout {
                id: body
                Layout.fillWidth: true
                visible: acc.expanded || !acc.collapsible
                spacing: Theme.s3
            }
        }
    }
    // 卡内二级折叠（「高级设置」）：比外层卡更轻 —— 没有边框、没有底色块，
    // 只有一行可点的标题 + 内容区。云端 LLM / 本地 LLM 各用一个实例，状态互不影响。
    component SubSection: ColumnLayout {
        id: section
        property bool expanded: false
        default property alias content: body.data
        Layout.fillWidth: true
        spacing: Theme.s2
        // 标题条自带底色 + 边框 + 悬浮高亮：之前在纯色背景上就是一行灰字，
        // 看不出这是能点开的入口。
        Rectangle {
            id: headBox
            Layout.fillWidth: true
            implicitHeight: sectionHead.implicitHeight + Theme.s2 * 2
            radius: Theme.rSm
            color: (headHover.hovered || section.expanded) ? Theme.surfaceHighest : Theme.surfaceHigh
            border.width: 1
            border.color: headHover.hovered ? Theme.outline : Theme.outlineVariant
            Behavior on color {
                ColorAnimation {
                    duration: Theme.durFast
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: Theme.durFast
                }
            }
            HoverHandler {
                id: headHover
                cursorShape: Qt.PointingHandCursor
            }
            TapHandler {
                onTapped: section.expanded = !section.expanded
            }
            RowLayout {
                id: sectionHead
                anchors.fill: parent
                anchors.leftMargin: Theme.s3
                anchors.rightMargin: Theme.s3
                spacing: Theme.s2
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: qsTr("高级设置")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeSmall
                    font.weight: Font.DemiBold
                    color: Theme.ink
                    Behavior on color {
                        ColorAnimation {
                            duration: Theme.durFast
                        }
                    }
                }
                Item {
                    Layout.fillWidth: true
                }
                Text {
                    Layout.alignment: Qt.AlignVCenter
                    text: section.expanded ? "\u25B4" : "\u25BE"
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.ink
                    Behavior on color {
                        ColorAnimation {
                            duration: Theme.durFast
                        }
                    }
                }
            }
        }
        // 展开后的内容也套一层更浅的底，和上面标题条连成一整块。
        Rectangle {
            id: bodyBox
            Layout.fillWidth: true
            visible: section.expanded
            implicitHeight: body.implicitHeight + Theme.s4 * 2
            radius: Theme.rSm
            color: Theme.surfaceLow
            border.width: 1
            border.color: Theme.outlineVariant
            ColumnLayout {
                id: body
                x: Theme.s4
                y: Theme.s4
                width: bodyBox.width - Theme.s4 * 2
                visible: section.expanded
                spacing: Theme.s3
            }
        }
    }
    // ---------------------------------------------------------------- 页面
    ScrollView {
        id: scroll
        anchors.fill: parent
        clip: true
        contentWidth: availableWidth
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
        ScrollBar.vertical.policy: ScrollBar.AsNeeded
        ColumnLayout {
            width: parent.width
            spacing: Theme.s4
            // -------------------------------------------------------- 页头
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                Layout.topMargin: Theme.s5
                spacing: Theme.s2
                Text {
                    text: qsTr("配置")
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeDisplay
                    font.weight: Font.Bold
                    color: Theme.ink
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("翻译链路怎么接通：选引擎、填凭据、装模型。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeBody
                    color: Theme.inkVariant
                }
            }
            // -------------------------------------------------------- 引擎（整行 · 常驻展开）
            CollapsibleCard {
                id: engineCard
                // 不折叠：这张卡本身就是一个分段控件 + 四行列表，收起等于把状态藏起来。
                collapsible: false
                expanded: true
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                title: qsTr("引擎")
                hint: qsTr("自动按优先级降级，强制只用一个；点行尾的状态胶囊跳到下面的对应配置。")
                // 模式 + 右上角说明。说明跟着模式走，别让人猜现在是哪一种。
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.s3
                    Segmented {
                        id: engineModeSeg
                        items: [
                            {
                                code: "auto",
                                label: qsTr("自动选择")
                            },
                            {
                                code: "force",
                                label: qsTr("强制引擎")
                            }
                        ]
                        current: page.forceMode ? "force" : "auto"
                        onSelected: function (code) {
                            page.setEngineMode(code === "force");
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Text {
                        Layout.alignment: Qt.AlignVCenter
                        text: page.forceMode ? (page.canonicalId(backend.engineId) === "auto" ? qsTr("当前：自动") : qsTr("当前：%1").arg(page.engineDisplayName(page.canonicalId(backend.engineId)))) : qsTr("自动 = 优先级，前一个不可用才轮下一个")
                        elide: Text.ElideRight
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeTiny
                        color: Theme.inkVariant
                    }
                }
                // 一份列表，形态跟着模式变：自动带序号且可拖，强制带单选圈且固定顺序。
                Item {
                    id: engineList
                    Layout.fillWidth: true
                    Layout.preferredHeight: engineRepeater.count * page.engineRowHeight
                    Repeater {
                        id: engineRepeater
                        model: page.forceMode ? page.engineKeys : page.autoOrder
                        delegate: Rectangle {
                            id: engineRow
                            required property int index
                            required property string modelData
                            readonly property bool autoMode: !page.forceMode
                            // 自动模式高亮「实际会被用到」的那个，强制模式高亮选中的那个。
                            readonly property bool highlighted: autoMode ? modelData === page.highlightEngineKey : page.engineSelected(modelData)
                            readonly property bool dragging: autoMode && page.dragIndex === index
                            // 被拖那行让出的空隙由其余行实时补位。
                            readonly property int shift: {
                                if (!page.autoModeDragging || dragging)
                                    return 0;
                                if (page.dragIndex < page.dragTarget && index > page.dragIndex && index <= page.dragTarget)
                                    return -page.engineRowHeight;
                                if (page.dragIndex > page.dragTarget && index >= page.dragTarget && index < page.dragIndex)
                                    return page.engineRowHeight;
                                return 0;
                            }
                            width: engineList.width
                            height: page.engineRowHeight - Theme.s2
                            y: index * page.engineRowHeight + (dragging ? page.dragOffset : shift)
                            z: dragging ? 2 : 0
                            radius: Theme.rMd
                            color: highlighted ? Theme.primaryContainer : Theme.surfaceLow
                            border.width: 1
                            border.color: highlighted || dragging ? Theme.primary : Theme.outlineVariant
                            Behavior on y {
                                enabled: !engineRow.dragging
                                NumberAnimation {
                                    duration: Theme.durFast
                                    easing.type: Easing.OutCubic
                                }
                            }
                            MouseArea {
                                anchors.fill: parent
                                // 手柄那一段让给 DragHandler，不然一按手柄就跳走。
                                anchors.leftMargin: engineRow.autoMode ? 46 : 0
                                cursorShape: Qt.PointingHandCursor
                                // 点行不再跳转：强制模式点行 = 选中该引擎；自动模式没有选中的概念。
                                // 要跳去配置，点行尾那颗状态胶囊。
                                onClicked: {
                                    if (!engineRow.autoMode)
                                        backend.engineId = page.engineCode(engineRow.modelData);
                                }
                            }
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: Theme.s2
                                anchors.rightMargin: Theme.s3
                                spacing: Theme.s2
                                // 拖动手柄：只有自动模式有，且只占左侧一小条。
                                Item {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: engineRow.autoMode
                                    implicitWidth: 20
                                    implicitHeight: 24
                                    Text {
                                        anchors.centerIn: parent
                                        text: "\u2807"
                                        font.family: Theme.fontMono
                                        font.pixelSize: Theme.fontSizeBody
                                        color: engineRow.dragging ? Theme.primary : (engineRow.highlighted ? Theme.primaryContainerText : Theme.inkVariant)
                                    }
                                    DragHandler {
                                        id: engineDrag
                                        target: null
                                        xAxis.enabled: false
                                        cursorShape: Qt.ClosedHandCursor
                                        onActiveChanged: {
                                            if (engineDrag.active)
                                                page.beginDrag(engineRow.index);
                                            else
                                                page.commitDrag();
                                        }
                                        onActiveTranslationChanged: {
                                            if (engineDrag.active)
                                                page.updateDrag(engineDrag.activeTranslation.y);
                                        }
                                    }
                                }
                                // 序号（自动）/ 单选圈（强制）二选一。
                                Text {
                                    Layout.alignment: Qt.AlignVCenter
                                    Layout.preferredWidth: 16
                                    visible: engineRow.autoMode
                                    text: engineRow.index + 1
                                    font.family: Theme.fontMono
                                    font.pixelSize: Theme.fontSizeBody
                                    font.weight: Font.DemiBold
                                    color: engineRow.highlighted ? Theme.primaryContainerText : Theme.inkVariant
                                }
                                Rectangle {
                                    Layout.alignment: Qt.AlignVCenter
                                    visible: !engineRow.autoMode
                                    implicitWidth: 18
                                    implicitHeight: 18
                                    radius: Theme.rFull
                                    color: "transparent"
                                    border.width: 1
                                    border.color: engineRow.highlighted ? Theme.primary : Theme.outline
                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: 10
                                        height: 10
                                        radius: Theme.rFull
                                        visible: engineRow.highlighted
                                        color: Theme.primary
                                    }
                                }
                                Image {
                                    Layout.alignment: Qt.AlignVCenter
                                    source: page.engineIcon(engineRow.modelData)
                                    sourceSize.width: 18
                                    sourceSize.height: 18
                                    // SVG 描边是写死的颜色，跟左栏一样用 MultiEffect 按主题重上色。
                                    layer.enabled: true
                                    layer.effect: MultiEffect {
                                        colorizationColor: engineRow.highlighted ? Theme.railIconActive : Theme.railIconIdle
                                        colorization: 1.0
                                    }
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignVCenter
                                    spacing: 0
                                    Text {
                                        Layout.fillWidth: true
                                        text: page.engineDisplayName(engineRow.modelData)
                                        elide: Text.ElideRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        font.weight: Font.DemiBold
                                        color: engineRow.highlighted ? Theme.primaryContainerText : Theme.ink
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: page.engineSubtitle(engineRow.modelData)
                                        elide: Text.ElideRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeTiny
                                        color: engineRow.highlighted ? Theme.primaryContainerText : Theme.inkVariant
                                        opacity: 0.85
                                    }
                                }
                                // 状态胶囊：行尾独立一颗可点的胶囊，点它才跳去这张卡的配置。
                                // 点行只负责「强制模式下选中」，两件事分开，误触能少一半。
                                //
                                // 配色跟翻译页底部的状态点同一套语义，**不跟主题强调色走**：
                                // 绿 = 就绪，黄 = 未就绪，红 = 缺资产。强调色是用户自己的品牌色，
                                // 拿它表达「能用 / 不能用」会读错。
                                Rectangle {
                                    id: statePill
                                    // 名字别叫 state：Item 自带 state 属性，同名重声明 QML 直接报错。
                                    readonly property string pillState: page.engineState(engineRow.modelData)
                                    readonly property color pillAccent: statePill.pillState === "ready" ? Theme.good : statePill.pillState === "missing-assets" ? Theme.danger : Theme.caution
                                    readonly property color pillFill: statePill.pillState === "ready" ? Theme.goodContainer : statePill.pillState === "missing-assets" ? Theme.dangerContainer : Theme.cautionContainer
                                    Layout.alignment: Qt.AlignVCenter
                                    implicitWidth: pillText.implicitWidth + Theme.s3 * 2
                                    implicitHeight: 22
                                    radius: Theme.rFull
                                    color: statePill.pillFill
                                    border.width: 1
                                    border.color: statePill.pillAccent
                                    Behavior on color {
                                        ColorAnimation {
                                            duration: Theme.durFast
                                        }
                                    }
                                    Text {
                                        id: pillText
                                        anchors.centerIn: parent
                                        text: page.engineStateLabel(engineRow.modelData)
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeTiny
                                        font.weight: Font.DemiBold
                                        color: statePill.pillAccent
                                    }
                                    MouseArea {
                                        anchors.fill: parent
                                        cursorShape: Qt.PointingHandCursor
                                        // 能解析 = 直接跳到它的配置卡；解析不了 = 先弹框说清原因，
                                        // 因为「跳过去看表格」治不了缺资产 / 源语言没选这两种病。
                                        onClicked: page.engineResolvable(engineRow.modelData) ? page.openEngineConfig(engineRow.modelData, false) : notReadyDialog.openFor(engineRow.modelData)
                                    }
                                }
                            }
                        }
                    }
                }
                Text {
                    Layout.fillWidth: true
                    text: qsTr("换了引擎或模型后，同样的原文可能得到不同的译文，旧译文不再复用。")
                    wrapMode: Text.WordWrap
                    font.family: Theme.fontUi
                    font.pixelSize: Theme.fontSizeTiny
                    color: Theme.inkVariant
                }
            }
            // -------------------------------------------- 引擎配置（4 个 tab 共用一张卡）
            CollapsibleCard {
                id: detailCard
                Layout.fillWidth: true
                Layout.leftMargin: Theme.s6
                Layout.rightMargin: Theme.s6
                // 常驻展开：里面是分段控件，折叠起来就看不出当前在配哪个引擎。
                collapsible: false
                expanded: true
                title: qsTr("引擎配置")
                hint: qsTr("这些设置只对当前这个引擎生效。")
                // 四个面板互斥切换。卡内的分段控件比四张竖排卡省一半纵向空间，
                // 也让「引擎 → 它的参数」变成一次点击，而不是先滚动定位。
                Segmented {
                    id: configTabBar
                    Layout.alignment: Qt.AlignLeft
                    items: [
                        {
                            code: "web",
                            label: backend.engineName("web")
                        },
                        {
                            code: "llm-cloud",
                            label: backend.engineName("llm-cloud")
                        },
                        {
                            code: "llm-local",
                            label: backend.engineName("llm-local")
                        },
                        {
                            code: "local-mt",
                            label: backend.engineName("local-mt")
                        }
                    ]
                    badges: page.configTabBadges
                    current: page.configTab
                    onSelected: function (code) {
                        page.configTab = code;
                    }
                }
                // ------------------------------------------------------------ Web 来源
                ColumnLayout {
                    id: webPanel
                    Layout.fillWidth: true
                    visible: page.configTab === "web"
                    opacity: visible ? 1.0 : 0.0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 320
                            easing.type: Easing.OutCubic
                        }
                    }
                    spacing: Theme.s3
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("单选；「自动」按 google → mymemory 依次尝试，固定某个来源后失败就直接报错，不会偷偷换源。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.inkVariant
                    }
                    Repeater {
                        model: page.webProviderOptions
                        delegate: ColumnLayout {
                            id: providerRow
                            required property int index
                            required property var modelData
                            readonly property bool selected: backend.webProviderId === modelData.id
                            Layout.fillWidth: true
                            spacing: Theme.s1
                            Item {
                                Layout.fillWidth: true
                                implicitHeight: 34
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: backend.webProviderId = providerRow.modelData.id
                                }
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: Theme.s1
                                    anchors.rightMargin: Theme.s1
                                    spacing: Theme.s2
                                    Rectangle {
                                        Layout.alignment: Qt.AlignVCenter
                                        implicitWidth: 18
                                        implicitHeight: 18
                                        radius: Theme.rFull
                                        color: "transparent"
                                        border.width: 1
                                        border.color: providerRow.selected ? Theme.primary : Theme.outline
                                        Rectangle {
                                            anchors.centerIn: parent
                                            width: 10
                                            height: 10
                                            radius: Theme.rFull
                                            visible: providerRow.selected
                                            color: Theme.primary
                                        }
                                    }
                                    Text {
                                        Layout.alignment: Qt.AlignVCenter
                                        Layout.fillWidth: true
                                        text: providerRow.modelData.displayName
                                        elide: Text.ElideRight
                                        font.family: Theme.fontUi
                                        font.pixelSize: Theme.fontSizeBody
                                        color: Theme.ink
                                    }
                                    // 需要 key 但还没配：提前说清楚，别等翻译时才报错。
                                    Badge {
                                        Layout.alignment: Qt.AlignVCenter
                                        visible: providerRow.modelData.requiresApiKey && !providerRow.modelData.configured
                                        text: qsTr("缺凭据")
                                        tone: "danger"
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                Layout.leftMargin: 26
                                visible: text.length > 0
                                text: providerRow.modelData.note
                                wrapMode: Text.WordWrap
                                font.family: Theme.fontUi
                                font.pixelSize: Theme.fontSizeTiny
                                color: Theme.inkVariant
                            }
                            // Azure 凭据：只有选中 azure 才展开。
                            ColumnLayout {
                                Layout.fillWidth: true
                                Layout.leftMargin: 26
                                visible: providerRow.modelData.id === "azure" && providerRow.selected
                                spacing: Theme.s2
                                FieldRow {
                                    label: "Key"
                                    ThemedField {
                                        font.family: Theme.fontMono
                                        placeholderText: qsTr("填入 Key")
                                        echoMode: TextInput.Password
                                    }
                                }
                                FieldRow {
                                    label: "Region"
                                    ThemedField {
                                        font.family: Theme.fontMono
                                        placeholderText: qsTr("例如 eastasia")
                                    }
                                }
                            }
                        }
                    }
                }
                // ------------------------------------------------------------ 云端 LLM
                ColumnLayout {
                    id: cloudPanel
                    Layout.fillWidth: true
                    visible: page.configTab === "llm-cloud"
                    opacity: visible ? 1.0 : 0.0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 320
                            easing.type: Easing.OutCubic
                        }
                    }
                    spacing: Theme.s3
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("填写服务商提供的地址与密钥；没填密钥时不会走云端翻译。")
                        wrapMode: Text.WordWrap
                        font.family: Theme.fontUi
                        font.pixelSize: Theme.fontSizeSmall
                        color: Theme.inkVariant
                    }
                    FieldRow {
                        label: qsTr("服务地址")
                        ThemedField {
                            font.family: Theme.fontMono
                            placeholderText: "http://127.0.0.1:3065/v1"
                            text: page.llmString("baseUrl")
                            // 键控提交而不是逐字提交：每个键都会落一次 QSettings + 重探全部引擎。
                            onEditingFinished: page.patchLlm("baseUrl", text)
                        }
                    }
                    FieldRow {
                        label: qsTr("密钥")
                        ThemedField {
                            font.family: Theme.fontMono
                            placeholderText: "sk-…"
                            echoMode: TextInput.Password
                            text: page.llmString("apiKey")
                            onEditingFinished: page.patchLlm("apiKey", text)
                        }
                    }
                    FieldRow {
                        label: qsTr("模型")
                        ThemedCombo {
                            id: cloudModelCombo
                            model: page.cloudModelOptions
                            currentIndex: page.cloudModelIndex()
                            onActivated: function (index) {
                                if (index >= 0 && index < page.cloudModelOptions.length)
                                    page.patchLlm("model", page.cloudModelOptions[index]);
                            }
                        }
                        AppButton {
                            text: qsTr("获取模型")
                            buttonEnabled: !backend.cloudBusy
                            onClicked: backend.fetchCloudModels()
                        }
                    }
                    Hint {
                        visible: text.length > 0
                        text: page.modelHint
                    }
                    // 上面三项（Base URL / API Key / 模型）是日常要动的；剩下八项进「高级设置」，
                    // 默认收起，省得一张卡把整页撑得没边。
                    SubSection {
                        FieldRow {
                            label: qsTr("等待上限")
                            // 上限跟 AppSettings 的钳位一致，否则存下去会被悄悄截断。
                            ThemedSpin {
                                from: 1000
                                to: 180000
                                stepSize: 1000
                                value: page.llmNumber("timeoutMs", 60000)
                                onValueModified: page.patchLlm("timeoutMs", value)
                            }
                        }
                        FieldRow {
                            label: qsTr("最长回复")
                            ThemedSpin {
                                from: 1
                                to: 32768
                                stepSize: 1
                                value: page.llmNumber("maxTokens", 2048)
                                onValueModified: page.patchLlm("maxTokens", value)
                            }
                        }
                        FieldRow {
                            label: qsTr("发散程度")
                            ThemedSpin {
                                from: 0
                                to: 150
                                stepSize: 5
                                value: Math.round(page.llmNumber("temperature", 0.2) * 100)
                                onValueModified: page.patchLlm("temperature", value / 100)
                                textFromValue: function (v) {
                                    return (v / 100).toFixed(2);
                                }
                                valueFromText: function (t) {
                                    var n = parseFloat(t);
                                    return isNaN(n) ? 0 : Math.round(n * 100);
                                }
                            }
                        }
                        FieldRow {
                            label: qsTr("风格要求")
                            TextArea {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 72
                                placeholderText: qsTr("留空使用默认风格")
                                selectByMouse: true
                                placeholderTextColor: Theme.inkVariant
                                selectionColor: Theme.primaryContainer
                                selectedTextColor: Theme.primaryContainerText
                                leftPadding: Theme.s3
                                rightPadding: Theme.s3
                                topPadding: Theme.s2
                                bottomPadding: Theme.s2
                                text: page.llmString("systemPrompt")
                                // TextArea 没有 editingFinished，失焦时才提交（同样是为了别逐字落盘）。
                                onActiveFocusChanged: {
                                    if (!activeFocus)
                                        page.patchLlm("systemPrompt", text);
                                }
                                background: Rectangle {
                                    radius: Theme.rMd
                                    border.width: parent.activeFocus ? 1 : 0
                                    border.color: Theme.primary
                                }
                            }
                        }
                        Hint {
                            text: qsTr("留空则使用默认的翻译风格。")
                        }
                        FieldRow {
                            label: qsTr("重试最长时间")
                            ThemedSpin {
                                from: 1000
                                to: 600000
                                stepSize: 1000
                                value: page.llmNumber("retryBudgetMs", 30000)
                                onValueModified: page.patchLlm("retryBudgetMs", value)
                            }
                        }
                        FieldRow {
                            label: qsTr("思考等级")
                            ThemedCombo {
                                id: effortCombo
                                model: [qsTr("关闭"), qsTr("极低"), qsTr("低"), qsTr("中"), qsTr("高"), qsTr("跟随默认")]
                                currentIndex: page.effortIndex(page.llmString("reasoningEffort"))
                                onActivated: function (index) {
                                    if (index >= 0 && index < page.effortCodes.length)
                                        page.patchLlm("reasoningEffort", page.effortCodes[index]);
                                }
                            }
                            AppButton {
                                text: qsTr("探测")
                                buttonEnabled: !backend.cloudBusy
                                onClicked: backend.probeCloudReasoning()
                            }
                        }
                        Hint {
                            visible: text.length > 0
                            text: page.probeHint
                        }
                        FieldRow {
                            label: qsTr("批量合并段数")
                            ThemedSpin {
                                from: 1
                                to: 64
                                stepSize: 1
                                value: page.llmNumber("batchSize", 8)
                                onValueModified: page.patchLlm("batchSize", value)
                            }
                        }
                        FieldRow {
                            label: qsTr("清理模型注记")
                            AppToggle {
                                Layout.alignment: Qt.AlignVCenter
                                checked: page.llmBool("stripModelNotes")
                                onToggled: function (checked) {
                                    page.patchLlm("stripModelNotes", checked);
                                    // 回灌：开关组件点击时先自己改了 checked，外部绑定被打破；
                                    // 以 llmDraft 为准把值拉回来，后端拒收/回落时界面才对得上。
                                    checked = page.llmBool("stripModelNotes");
                                }
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: qsTr("清掉小模型加的「【中文】」注记，只删整行括注，正文不动。")
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                }
                // ---------------------------------------- 本地 LLM（llama-server）
                ColumnLayout {
                    id: localPanel
                    Layout.fillWidth: true
                    visible: page.configTab === "llm-local"
                    opacity: visible ? 1.0 : 0.0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 320
                            easing.type: Easing.OutCubic
                        }
                    }
                    spacing: Theme.s3
                    // 服务在跑的时候定时抓一次日志尾部：长翻译过程中日志一直在长，
                    // 只靠启动 / 结束两个时间点刷新，看起来就像"没有日志"。
                    Timer {
                        interval: 1500
                        repeat: true
                        running: backend.localPid > 0 && page.visible
                        onTriggered: backend.refreshLocalState()
                    }
                    // 模型或 llama 运行时缺了也是同一个去处：设置页资产分区的 llm 分类。
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s2
                        Text {
                            color: Theme.ink
                            Layout.fillWidth: true
                            text: qsTr("启动时界面会短暂等待；失败会自动清理。")
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignTop
                            text: qsTr("下载 / 导入")
                            onClicked: page.openAssetsRequested("llm")
                        }
                    }
                    FieldRow {
                        label: qsTr("模型文件")
                        ThemedCombo {
                            id: localModelCombo
                            model: page.localModelOptions
                            currentIndex: Math.max(0, page.localModelOptions.indexOf(backend.localModel))
                            onActivated: function (index) {
                                // 第一项是「自动」= 空串，后端拿第一个可用模型。
                                backend.localModel = index <= 0 ? "" : page.localModelOptions[index];
                            }
                        }
                        AppButton {
                            text: qsTr("浏览…")
                            // 后端 installFromFile 是按 manifest 里的资产 id 校验 sha256 的，
                            // 随便挑一个 .gguf 过不了校验，所以这里仍然只保留接口。
                            onClicked: page.importHint = qsTr("暂时只能从「资产」页下载；手动导入文件稍后开放。")
                        }
                    }
                    Hint {
                        visible: text.length > 0
                        text: page.importHint
                    }
                    // 运行时可用性：后端问了 llama-server 的 exe 在不在，界面如实显示。
                    // 装了哪个就点亮哪个（绿色实心），没装的是空心灰边。
                    FieldRow {
                        label: qsTr("可用的加速组件")
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s2
                            Badge {
                                text: "Vulkan"
                                filled: backend.localGpuAvailable
                                tone: filled ? "good" : "neutral"
                            }
                            Badge {
                                text: "CPU"
                                filled: backend.localCpuAvailable
                                tone: filled ? "good" : "neutral"
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: backend.localGpuAvailable || backend.localCpuAvailable ? qsTr("绿色 = 已安装") : qsTr("还没安装加速组件，先到「资产」页下载")
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                    // 只装了 Vulkan 运行时的时候，关掉「优先 GPU」不会省显卡，
                    // 只会让引擎去找并不存在的 CPU 版 exe 然后起不来 —— 所以锁成常开。
                    FieldRow {
                        label: qsTr("优先 GPU")
                        AppToggle {
                            Layout.alignment: Qt.AlignVCenter
                            checked: backend.localGpuLocked ? true : backend.localUseGpu
                            // 用组件自己的 toggleEnabled 置灰并挡点击：直接写 enabled/opacity
                            // 会盖掉 AppToggle 内部那套透明度，开关看着像"关着"。
                            toggleEnabled: !backend.localGpuLocked
                            onToggled: function (checked) {
                                backend.localUseGpu = checked;
                                // 回灌：开关组件内部先改了自己的 checked，绑定已被打断，
                                // 这里按后端实际值拉回来（后端可能因不同平台拒绝）。
                                checked = backend.localUseGpu;
                            }
                        }
                        Text {
                            color: Theme.ink
                            Layout.fillWidth: true
                            Layout.minimumWidth: 0
                            text: backend.localGpuLocked
                                  ? qsTr("只装了 Vulkan 运行时时必须开启；想纯 CPU 跑，把下面的「GPU 层数」填 0。")
                                  : qsTr("有显卡加速就用显卡；关掉只用 CPU。")
                            wrapMode: Text.WordWrap
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    // 这四项直接进 LlamaServerOptions（contextSize / threads / gpuLayers / extraArgs），
                    // 属于调参项，收进「高级设置」；默认值就能跑。
                    // 改完不会立刻重启服务：下次真正要用本地引擎时 ensureStarted() 拿新配置
                    // 和正在跑的那份比，不一致才重启。
                    SubSection {
                        expanded: true // TEMP-SHOT
                        // ---- 服务生命周期（默认：拉起后一直留到软件关闭） ----
                        FieldRow {
                            label: qsTr("切换引擎时关闭")
                            AppToggle {
                                Layout.alignment: Qt.AlignVCenter
                                checked: backend.localStopOnSwitch
                                onToggled: function (checked) {
                                    backend.localStopOnSwitch = checked;
                                    checked = backend.localStopOnSwitch;
                                }
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                // 说明文字很长：不给最小值，否则它会把整张卡撑得比列还宽。
                                Layout.minimumWidth: 0
                                text: qsTr("打开后，切到别的翻译引擎就把 llama-server 停掉；关着则一直留到软件退出。")
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("启动时预热")
                            AppToggle {
                                Layout.alignment: Qt.AlignVCenter
                                checked: backend.localPreheat
                                onToggled: function (checked) {
                                    backend.localPreheat = checked;
                                    checked = backend.localPreheat;
                                }
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: qsTr("打开软件就把本地翻译准备好，第一次翻译不用等；代价是常驻占内存。")
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("空闲自动停")
                            AppToggle {
                                Layout.alignment: Qt.AlignVCenter
                                checked: backend.localIdleStop
                                // 「切换引擎时关闭」打开时，离开本地引擎立刻会停，
                                // 空闲这套就没有意义了 —— 置灰，但仍然记住用户的勾选。
                                enabled: !backend.localStopOnSwitch
                                opacity: enabled ? 1.0 : 0.45
                                onToggled: function (checked) {
                                    backend.localIdleStop = checked;
                                    checked = backend.localIdleStop;
                                }
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: backend.localStopOnSwitch ? qsTr("已关闭：上面那个开关打开时，切走就会停，不需要等空闲。") : qsTr("一段时间不用就自动停掉，省内存。")
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("空闲阈值")
                            ThemedSpin {
                                from: 1
                                to: 240
                                stepSize: 1
                                value: backend.localIdleMinutes
                                enabled: backend.localIdleStop && !backend.localStopOnSwitch
                                opacity: enabled ? 1.0 : 0.45
                                onValueModified: backend.localIdleMinutes = value
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                Layout.minimumWidth: 0
                                text: qsTr("分钟。只在「空闲自动停」打开、且「切换引擎时关闭」关着时生效。")
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("上下文")
                            ThemedSpin {
                                from: 512
                                to: 32768
                                stepSize: 512
                                value: backend.localContextSize
                                onValueModified: backend.localContextSize = value
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: qsTr("一次能读入的最大长度。太小装不下长句，太大浪费内存。")
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("线程数")
                            ThemedSpin {
                                from: 0
                                to: 64
                                stepSize: 1
                                value: backend.localThreads
                                onValueModified: backend.localThreads = value
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: qsTr("0 表示自动。")
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("GPU 层数")
                            ThemedSpin {
                                id: gpuSpin
                                from: -1
                                to: 999
                                stepSize: 1
                                value: backend.localGpuLayers
                                onValueModified: backend.localGpuLayers = value
                                ToolTip.visible: gpuHover.hovered
                                ToolTip.text: qsTr("-1 = 自动（不传 -ngl，由 llama.cpp 按空闲显存挑层数）；0 = 不卸载任何层，纯 CPU 算")
                                HoverHandler {
                                    id: gpuHover
                                }
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: qsTr("-1 = 自动，对应 -ngl。显存不够时手动降档（留空/自动就交给 llama.cpp 自己挑）。")
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                        FieldRow {
                            label: qsTr("附加选项")
                            ThemedField {
                                id: extraArgsField
                                placeholderText: qsTr("留空则不追加（例如 --no-warmup --flash-attn）")
                                // 回填只做一次：QML 的 TextField 一旦被用户输入就会打断绑定，
                                // 所以不复用 text: backend.localExtraArgs，而是提交时写回。
                                Component.onCompleted: text = backend.localExtraArgs
                                onEditingFinished: backend.localExtraArgs = text
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: qsTr("会原样传给本地引擎，一般不用填。")
                                elide: Text.ElideRight
                                verticalAlignment: Text.AlignVCenter
                            }
                        }
                    }
                    // 采样参数的归属说明：本地引擎沿用云端卡那份 temperature / maxTokens /
                    // 系统提示词，只覆盖 batchSize / reasoningEffort / stripModelNotes 三项。
                    Hint {
                        text: qsTr("回复风格沿用「云端 LLM」里的设置；本地模型按段翻译，并会去掉多余的注释。")
                    }
                    // 状态行：后端 / 端口 / PID / 启动耗时 + 启动 / 停止
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s3
                        StatusDot {
                            Layout.alignment: Qt.AlignVCenter
                            label: page.localLlmReady ? qsTr("运行中") : qsTr("已停止")
                            state: page.localLlmReady ? 0 : 3
                        }
                        Rectangle {
                            Layout.alignment: Qt.AlignVCenter
                            Layout.preferredWidth: 1
                            Layout.preferredHeight: 16
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("运行方式")
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: backend.localBackend === "vulkan" ? qsTr("显卡加速") : (backend.localBackend === "cpu" ? "CPU" : (backend.localBackend.length > 0 ? backend.localBackend : "—"))
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("本地端口")
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: backend.localPort > 0 ? backend.localPort : "—"
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("进程号")
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: backend.localPid > 0 ? backend.localPid : "—"
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("启动耗时")
                        }
                        Text {
                            color: Theme.ink
                            Layout.alignment: Qt.AlignVCenter
                            text: backend.localStartupMs > 0 ? qsTr("%1 ms").arg(backend.localStartupMs) : "—"
                        }
                        Item {
                            Layout.fillWidth: true
                        }
                        // 缺资产时不给点「启动」：点了也只会拿到一条看不懂的失败。
                        AppButton {
                            Layout.alignment: Qt.AlignVCenter
                            visible: !page.localCanStart
                            text: qsTr("去「资产」页")
                            onClicked: page.openAssetsRequested("llm")
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("启动")
                            variant: "primary"
                            buttonEnabled: page.localCanStart && !page.localLlmReady
                            onClicked: backend.startLocalServer()
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignVCenter
                            text: qsTr("停止")
                            buttonEnabled: page.localLlmReady
                            onClicked: backend.stopLocalServer()
                        }
                    }
                    Hint {
                        visible: !page.localCanStart
                        text: {
                            if (!page.localAssetsReady && !page.localHasModel)
                                return qsTr("还缺加速组件和模型文件：到「资产」页下载。");
                            if (!page.localAssetsReady)
                                return qsTr("还缺加速组件：到「资产」页下载显卡版或 CPU 版。");
                            return qsTr("还缺模型文件：到「资产」页下载一个即可。");
                        }
                    }
                    // 日志：路径 + 尾部。后端 logTail() 读的就是这个文件，
                    // 启动失败时这里通常是唯一能看到原因的地方。
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s1
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s2
                            Text {
                                color: Theme.ink
                                text: qsTr("日志")
                            }
                            Text {
                                color: Theme.ink
                                Layout.fillWidth: true
                                text: backend.localLogPath.length > 0 ? backend.localLogPath : qsTr("（还没有日志文件）")
                                elide: Text.ElideMiddle
                            }
                            AppButton {
                                text: qsTr("刷新")
                                onClicked: backend.refreshLocalState()
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 120
                            radius: Theme.rMd
                            border.width: 1
                            border.color: Theme.outlineVariant
                            clip: true
                            Flickable {
                                id: logFlick
                                anchors.fill: parent
                                anchors.margins: Theme.s2
                                clip: true
                                contentWidth: logEdit.width
                                contentHeight: logEdit.height
                                boundsBehavior: Flickable.StopAtBounds
                                // 日志只会越写越多：新内容进来就贴到底，否则永远盯着一小时前那段。
                                function stickToBottom() {
                                    contentY = Math.max(0, contentHeight - height);
                                }
                                TextEdit {
                                    id: logEdit
                                    width: Math.max(logFlick.width, implicitWidth)
                                    readOnly: true
                                    selectByMouse: true
                                    text: backend.localLogTail.length > 0 ? backend.localLogTail : qsTr("启动本地 llama-server 后，这里显示它最近的输出。")
                                    selectionColor: Theme.primaryContainer
                                    selectedTextColor: Theme.primaryContainerText
                                    // 文本换完、高度算完才滚，否则拿到的是旧 contentHeight。
                                    onTextChanged: Qt.callLater(logFlick.stickToBottom)
                                    Component.onCompleted: Qt.callLater(logFlick.stickToBottom)
                                }
                            }
                        }
                    }
                }
                // ------------------------------------------------------------ 本地 MT
                ColumnLayout {
                    id: mtPanel
                    Layout.fillWidth: true
                    visible: page.configTab === "local-mt"
                    opacity: visible ? 1.0 : 0.0
                    Behavior on opacity {
                        NumberAnimation {
                            duration: 320
                            easing.type: Easing.OutCubic
                        }
                    }
                    spacing: Theme.s3
                    // 一个语言对都没装时的正路：交给 Main.qml 切到设置页的资产分区（mt）。
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s2
                        Text {
                            color: Theme.ink
                            Layout.fillWidth: true
                            text: qsTr("按顺序使用第一个可用的语言对；经英文中转会慢一些。")
                        }
                        AppButton {
                            Layout.alignment: Qt.AlignTop
                            text: qsTr("下载 / 导入")
                            onClicked: page.openAssetsRequested("mt")
                        }
                    }
                    // ---- 常驻策略 ----
                    // 本地 MT 没有独立进程，这里的开关管的是「权重什么时候加载进内存」。
                    // 一个语言库都没装时整组置灰：开了也没东西可加载。
                    FieldRow {
                        label: qsTr("开机自启")
                        enabled: backend.mtPacksInstalled
                        opacity: enabled ? 1.0 : 0.45
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s3
                            AppToggle {
                                checked: backend.mtPreload
                                onToggled: function (checked) {
                                    backend.mtPreload = checked;
                                    checked = backend.mtPreload;
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Text {
                                color: Theme.ink
                                text: backend.mtLoadedPairs > 0 ? qsTr("常驻 %1 个语言对").arg(backend.mtLoadedPairs) : qsTr("未加载")
                            }
                        }
                    }
                    Hint {
                        font.pixelSize: Theme.fontSizeSmall
                        text: qsTr("打开后，软件一启动就把已装的语言对全部加载好，第一次翻译不用等加载；关着则谁用谁加载。")
                    }
                    FieldRow {
                        label: qsTr("空闲释放")
                        enabled: backend.mtPacksInstalled && backend.mtPreload
                        opacity: enabled ? 1.0 : 0.45
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.s3
                            ThemedSpin {
                                Layout.fillWidth: false
                                Layout.preferredWidth: 120
                                from: 0
                                to: 180
                                stepSize: 1
                                value: backend.mtIdleMinutes
                                onValueModified: backend.mtIdleMinutes = value
                            }
                            Text {
                                color: Theme.ink
                                text: qsTr("分钟")
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Text {
                                color: Theme.ink
                                text: backend.mtIdleMinutes === 0 ? qsTr("不自动释放") : qsTr("到点释放内存")
                            }
                        }
                    }
                    Hint {
                        font.pixelSize: Theme.fontSizeSmall
                        text: qsTr("这段时间没有离线翻译就释放占用的内存；下次再用会重新加载（约几百毫秒）。填 0 表示一直留着。")
                    }
                    // 表头
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.s3
                        Text {
                            color: Theme.ink
                            Layout.preferredWidth: 120
                            text: qsTr("语言对")
                            font.weight: Font.DemiBold
                        }
                        Text {
                            color: Theme.ink
                            Layout.fillWidth: true
                            text: qsTr("保存位置")
                            font.weight: Font.DemiBold
                        }
                        Text {
                            color: Theme.ink
                            Layout.preferredWidth: 96
                            horizontalAlignment: Text.AlignRight
                            text: qsTr("状态")
                            font.weight: Font.DemiBold
                        }
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 1
                    }
                    Repeater {
                        // 后端现扫磁盘（installedPairs + missingFiles），不再由 QML 硬编码表。
                        model: backend.mtPairs
                        delegate: RowLayout {
                            id: mtRow
                            required property var modelData
                            Layout.fillWidth: true
                            spacing: Theme.s3
                            Text {
                                color: Theme.ink
                                Layout.preferredWidth: 120
                                text: mtRow.modelData.pair !== undefined ? mtRow.modelData.pair : ""
                                elide: Text.ElideRight
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Text {
                                    color: Theme.ink
                                    Layout.fillWidth: true
                                    text: mtRow.modelData.path !== undefined ? mtRow.modelData.path : ""
                                    elide: Text.ElideRight
                                }
                                // 目录在 ≠ 能用：缺 config.json / vocab.json 一样加载不了。
                                Text {
                                    color: Theme.ink
                                    Layout.fillWidth: true
                                    visible: mtRow.modelData.usable === false
                                    text: mtRow.modelData.missing !== undefined ? qsTr("缺少：%1").arg(mtRow.modelData.missing.join("、")) : ""
                                }
                            }
                            Badge {
                                Layout.preferredWidth: 96
                                Layout.alignment: Qt.AlignRight | Qt.AlignTop
                                text: mtRow.modelData.usable === true ? qsTr("完整") : qsTr("不可用")
                                tone: mtRow.modelData.usable === true ? "neutral" : "danger"
                            }
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        visible: backend.mtPairs.length === 0
                        text: qsTr("还没安装语言包：到「资产」页下载。")
                        // 显式给色：默认走 QPalette，深色主题下拿到的还是浅色那套，字会变黑。
                        color: Theme.dark ? "#FFFFFF" : Theme.inkVariant
                        opacity: 0.85
                    }
                    // 当前语言对真正会走哪条路 —— 后端算的，不是猜的。
                    // 英文中转要跑两次推理，这里如实写出来，省得用户以为「多语言模型」和直连一样快。
                    Text {
                        id: mtRouteNote
                        readonly property string route: {
                            const dst = backend.dstLangs.length > 0 ? backend.dstLangs[0] : "";
                            return backend.mtRouteFor(backend.srcLang, dst);
                        }
                        Layout.fillWidth: true
                        text: mtRouteNote.route.length > 0 ? qsTr("当前语言对走：%1").arg(mtRouteNote.route) : qsTr("这个语言方向还不能离线翻译；本地翻译需要先选定源语言，不能选「自动检测」。")
                        color: Theme.dark ? "#FFFFFF" : Theme.inkVariant
                    }
                    // ONNX Runtime 是编译期链接、随安装包分发的（exe 同目录的 onnxruntime.dll），
                    // 不走资产下载。写这句是为了让人别再来资产页找本地 MT 的「驱动」。
                    Text {
                        Layout.fillWidth: true
                        text: qsTr("配套组件已内置，无需另外下载。")
                        color: Theme.dark ? "#FFFFFF" : Theme.inkVariant
                        opacity: 0.85
                    }
                }
                Item {
                    Layout.preferredHeight: Theme.s6
                }
            }
        }
        // 「这个引擎当前还用不了」拦截框。点解析不了的状态胶囊会调它；
        // 点行 / 框里的「去配置」都走 guard = false，不再拦第二次。
        Popup {
            id: notReadyDialog
            property string engineKey: ""
            function openFor(key) {
                engineKey = key;
                open();
            }
            parent: Overlay.overlay
            anchors.centerIn: parent
            width: Math.min(420, parent ? parent.width - Theme.s6 * 2 : 420)
            modal: true
            focus: true
            padding: Theme.s5
            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
            background: Rectangle {
                radius: Theme.rLg
                border.width: 1
                border.color: Theme.outlineVariant
            }
            contentItem: ColumnLayout {
                spacing: Theme.s2
                Text {
                    color: Theme.ink
                    Layout.fillWidth: true
                    text: qsTr("%1 还没有配置好").arg(page.engineDisplayName(notReadyDialog.engineKey))
                    font.weight: Font.DemiBold
                }
                Text {
                    color: Theme.ink
                    Layout.fillWidth: true
                    // 拦截原因以后端给的 detail 为准（缺资产 / 选定源语言后才可用 / 参数没填齐，
                    // 三种说法都在 detail 里），取不到才退回到一句通用说明。
                    text: {
                        var detail = page.engineSubtitle(notReadyDialog.engineKey);
                        if (detail.length > 0)
                            return detail;
                        return page.engineState(notReadyDialog.engineKey) === "missing-assets" ? qsTr("缺少必需的文件，先到「资产」页下载。") : qsTr("当前语言对下还用不了它，先检查一下配置。");
                    }
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.s4
                    spacing: Theme.s2
                    Item {
                        Layout.fillWidth: true
                    }
                    AppButton {
                        text: qsTr("取消")
                        onClicked: notReadyDialog.close()
                    }
                    AppButton {
                        text: qsTr("去「资产」页")
                        visible: page.engineState(notReadyDialog.engineKey) === "missing-assets"
                        onClicked: {
                            var key = notReadyDialog.engineKey;
                            notReadyDialog.close();
                            page.openAssetsRequested(key === "local-mt" ? "mt" : "llm");
                        }
                    }
                    AppButton {
                        text: qsTr("去配置")
                        variant: "primary"
                        onClicked: {
                            var key = notReadyDialog.engineKey;
                            notReadyDialog.close();
                            page.openEngineConfig(key, false);
                        }
                    }
                }
            }
        }
    }
    }
