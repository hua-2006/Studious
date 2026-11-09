#include "CacheHandle.h"

#include "AssetPaths.h"

namespace studious::ui {

studious::cache::TranslationCache &sharedCache()
{
    // 函数内静态 + 裸指针：寿命覆盖整个进程，线程安全由 C++11 的静态初始化保证。
    static studious::cache::TranslationCache *instance = nullptr;
    if (instance == nullptr) {
        // 路径口径统一在 AssetPaths 里（默认 <dataRoot>/cache.db）。
        instance = new studious::cache::TranslationCache(paths::cacheDbPath());
    }
    return *instance;
}

}   // namespace studious::ui
