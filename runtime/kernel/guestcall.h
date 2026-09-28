#pragma once

#include <array>
#include <cstdint>
#include <tuple>
#include <type_traits>
#include <utility>

#include <xbox.h>

#include "klog.h"
#include "memory.h"
#include "ppc_recomp_shared.h"

// Imports occasionally need access to r13/lr or need to perform a nested guest
// call. Every host thread currently executing translated code exposes its active
// PPC context here.
inline thread_local PPCContext* g_ppcContext = nullptr;

namespace mojorecomp::guestcall {

inline uint64_t GetGpr(const PPCContext& ctx, uint8_t* base, size_t pos) noexcept
{
    switch (pos)
    {
    case 0: return ctx.r3.u64;
    case 1: return ctx.r4.u64;
    case 2: return ctx.r5.u64;
    case 3: return ctx.r6.u64;
    case 4: return ctx.r7.u64;
    case 5: return ctx.r8.u64;
    case 6: return ctx.r9.u64;
    case 7: return ctx.r10.u64;
    default:
        // Xenon ABI parameter save area. Later integer arguments occupy an
        // eight-byte slot but are represented by a big-endian 32-bit word.
        return *reinterpret_cast<be<uint32_t>*>(
            base + ctx.r1.u32 + 0x54 + (pos - 8) * 8);
    }
}

inline double GetFpr(const PPCContext& ctx, size_t ordinal) noexcept
{
    switch (ordinal)
    {
    case 0: return ctx.f1.f64;
    case 1: return ctx.f2.f64;
    case 2: return ctx.f3.f64;
    case 3: return ctx.f4.f64;
    case 4: return ctx.f5.f64;
    case 5: return ctx.f6.f64;
    case 6: return ctx.f7.f64;
    case 7: return ctx.f8.f64;
    case 8: return ctx.f9.f64;
    case 9: return ctx.f10.f64;
    case 10: return ctx.f11.f64;
    case 11: return ctx.f12.f64;
    default: return ctx.f13.f64;
    }
}

template<typename T>
T FetchArg(PPCContext& ctx, uint8_t* base, size_t pos, size_t floatOrdinal) noexcept
{
    if constexpr (std::is_floating_point_v<T>)
    {
        return static_cast<T>(GetFpr(ctx, floatOrdinal));
    }
    else if constexpr (std::is_pointer_v<T>)
    {
        const uint32_t guest = static_cast<uint32_t>(GetGpr(ctx, base, pos));
        return guest ? reinterpret_cast<T>(base + guest) : nullptr;
    }
    else if constexpr (sizeof(T) == 8)
    {
        return static_cast<T>(GetGpr(ctx, base, pos));
    }
    else
    {
        return static_cast<T>(static_cast<uint32_t>(GetGpr(ctx, base, pos)));
    }
}

template<typename... Args>
constexpr auto FloatOrdinals()
{
    std::array<size_t, sizeof...(Args) + 1> ordinals{};
    size_t next = 0;
    size_t i = 0;
    ((ordinals[i++] = std::is_floating_point_v<Args> ? next++ : 0), ...);
    return ordinals;
}

template<typename R, typename... Args, size_t... I>
void DispatchImpl(R (*func)(Args...), PPCContext& ctx, uint8_t* base,
                  std::index_sequence<I...>)
{
    constexpr auto ordinals = FloatOrdinals<Args...>();
    std::tuple<Args...> args{FetchArg<Args>(ctx, base, I, ordinals[I])...};

    if constexpr (std::is_void_v<R>)
    {
        std::apply(func, args);
    }
    else
    {
        R value = std::apply(func, args);
        if constexpr (std::is_pointer_v<R>)
            ctx.r3.u64 = value ? g_guestMemory.MapVirtual(value) : 0;
        else if constexpr (std::is_floating_point_v<R>)
            ctx.f1.f64 = static_cast<double>(value);
        else
            ctx.r3.u64 = static_cast<uint64_t>(value);
    }
}

template<typename R, typename... Args>
constexpr std::tuple<Args...> ArgsOf(R (*)(Args...))
{
    return {};
}

template<auto Func>
void Dispatch(PPCContext& ctx, uint8_t* base)
{
    g_ppcContext = &ctx;
    DispatchImpl(Func, ctx, base,
                 std::make_index_sequence<
                     std::tuple_size_v<decltype(ArgsOf(Func))>>{});
}

} // namespace mojorecomp::guestcall

#define GUEST_FUNCTION_HOOK(guest, host)                                                \
    PPC_FUNC(guest)                                                                     \
    {                                                                                   \
        KCALL(#guest);                                                                  \
        mojorecomp::guestcall::Dispatch<host>(ctx, base);                                 \
    }

// Only use for calls where a no-op success is the faithful platform behavior.
#define GUEST_FUNCTION_STUB(guest)                                                       \
    PPC_FUNC(guest)                                                                     \
    {                                                                                   \
        KCALL(#guest);                                                                  \
        ctx.r3.u64 = 0;                                                                 \
    }
