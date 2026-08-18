#pragma once

#include <functional>
#include <type_traits>
#include <utility>

namespace rmdb {

// 作用域退出时执行清理动作；调用 release() 可取消执行。清理动作必须保证不抛异常。
template <typename Fn>
class ScopeExit {
   public:
    explicit ScopeExit(Fn fn) noexcept(std::is_nothrow_move_constructible_v<Fn>) : fn_(std::move(fn)) {}

    ScopeExit(const ScopeExit &) = delete;
    ScopeExit &operator=(const ScopeExit &) = delete;
    ScopeExit &operator=(ScopeExit &&) = delete;

    ScopeExit(ScopeExit &&other) noexcept(std::is_nothrow_move_constructible_v<Fn>)
        : fn_(std::move(other.fn_)), active_(std::exchange(other.active_, false)) {}

    ~ScopeExit() noexcept {
        if (active_) {
            std::invoke(fn_);
        }
    }

    void release() noexcept { active_ = false; }

   private:
    Fn fn_;
    bool active_{true};
};

template <typename Fn>
[[nodiscard]] auto make_scope_exit(Fn &&fn) {
    return ScopeExit<std::decay_t<Fn>>(std::forward<Fn>(fn));
}

}  // namespace rmdb
