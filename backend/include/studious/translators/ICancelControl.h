#pragma once

namespace studious::translate {

// 只给 Router 用的"清取消标志"能力。
//
// 为什么不做成 core::ITranslator 的成员：取消是**整单请求级**语义。
//   cancel() 由 UI 线程随时调用，Router 降级时会连续调多个引擎的 translate()。
//   如果每个引擎在自己的 translate() 入口清 m_cancelRequested（旧行为），那么
//   "上一个引擎刚失败、用户刚好在这一刻点取消"这个窗口里的取消会被下一个引擎
//   在入口直接吞掉 —— 用户点了取消，界面却继续翻完。
//   所以清零只能由 Router 在每个 translate() 请求开始时做一次，引擎自己一律不清。
//
// 放在独立的小接口里而不是改契约头：contract/ 是跨模块契约，加虚函数会波及
// 所有实现方；这里用 dynamic_cast 探测，没实现该接口的第三方引擎照旧工作
// （它们仍按老办法自己清，不会因为缺接口就编译不过）。
class ICancelControl
{
public:
    virtual ~ICancelControl() = default;

    // 清掉自己的取消标志。只允许 Router 在请求开始时调用。
    virtual void resetCancel() = 0;
};

} // namespace studious::translate
