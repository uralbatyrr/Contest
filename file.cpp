#pragma once

#include <cstddef>
#include <functional>
#include <new>
#include <type_traits>
#include <typeinfo>
#include <utility>

template <bool Copyable, typename Derived, typename T>
class FunctionBase;

template <bool Copyable, typename Derived, typename Ret, typename... Args>
class FunctionBase<Copyable, Derived, Ret(Args...)> {
 private:
  static constexpr size_t kBufferSize = 16;

  template <typename F>
  static constexpr bool kIsSmall = sizeof(F) <= kBufferSize &&
                                   alignof(F) <= alignof(std::max_align_t) &&
                                   std::is_nothrow_move_constructible_v<F>;

  template <typename F>
  static constexpr bool kIsCallable =
      !std::is_same_v<std::decay_t<F>, Derived> &&
      std::is_invocable_r_v<Ret, std::decay_t<F>&, Args...>;

  struct Operations {
    void (*destroy)(void*);
    void* (*move)(void*, void*);
    void* (*copy)(const void*, void*);
    const std::type_info& (*type)();
  };

  template <typename F>
  static Ret Invoke(void* object, Args... args) {
    return static_cast<Ret>(
        std::invoke(*static_cast<F*>(object), std::forward<Args>(args)...));
  }

  template <typename F>
  static void Destroy(void* object) {
    if constexpr (kIsSmall<F>) {
      static_cast<F*>(object)->~F();
    } else {
      delete static_cast<F*>(object);
    }
  }

  template <typename F>
  static void* Move(void* object, void* buffer) {
    if constexpr (kIsSmall<F>) {
      F* moved = new (buffer) F(std::move(*static_cast<F*>(object)));
      static_cast<F*>(object)->~F();
      return moved;
    } else {
      return object;
    }
  }

  template <typename F>
  static void* Copy(const void* object, void* buffer) {
    if constexpr (Copyable) {
      const F& source = *static_cast<const F*>(object);
      if constexpr (kIsSmall<F>) {
        return new (buffer) F(source);
      } else {
        return new F(source);
      }
    } else {
      return nullptr;
    }
  }

  template <typename F>
  static const std::type_info& Type() {
    return typeid(F);
  }

  template <typename F>
  static constexpr Operations kOperations = {&Destroy<F>, &Move<F>, &Copy<F>,
                                             &Type<F>};

  alignas(std::max_align_t) char buffer_[kBufferSize];
  void* object_ = nullptr;
  Ret (*invoke_)(void*, Args...) = nullptr;
  const Operations* operations_ = nullptr;

  void Reset() {
    object_ = nullptr;
    invoke_ = nullptr;
    operations_ = nullptr;
  }

  void Clear() {
    if (object_ != nullptr) {
      operations_->destroy(object_);
    }
    Reset();
  }

  void Adopt(FunctionBase&& other) {
    if (other.object_ != nullptr) {
      object_ = other.operations_->move(other.object_, buffer_);
    }
    invoke_ = other.invoke_;
    operations_ = other.operations_;
    other.Reset();
  }

 public:
  FunctionBase() = default;

  FunctionBase(std::nullptr_t) {}

  template <typename F>
    requires kIsCallable<F>
  FunctionBase(F&& function) {
    using Stored = std::decay_t<F>;
    if constexpr (kIsSmall<Stored>) {
      object_ = new (buffer_) Stored(std::forward<F>(function));
    } else {
      object_ = new Stored(std::forward<F>(function));
    }
    invoke_ = &Invoke<Stored>;
    operations_ = &kOperations<Stored>;
  }

  FunctionBase(const FunctionBase& other)
      : invoke_(other.invoke_), operations_(other.operations_) {
    if (other.object_ != nullptr) {
      object_ = operations_->copy(other.object_, buffer_);
    }
  }

  FunctionBase(FunctionBase&& other) { Adopt(std::move(other)); }

  FunctionBase& operator=(const FunctionBase& other) {
    FunctionBase copy = other;
    Clear();
    Adopt(std::move(copy));
    return *this;
  }

  FunctionBase& operator=(FunctionBase&& other) {
    if (this != &other) {
      Clear();
      Adopt(std::move(other));
    }
    return *this;
  }

  template <typename F>
    requires kIsCallable<F>
  Derived& operator=(F&& function) {
    *this = FunctionBase(std::forward<F>(function));
    return static_cast<Derived&>(*this);
  }

  template <typename F>
  Derived& operator=(std::reference_wrapper<F> reference) {
    *this = FunctionBase(reference);
    return static_cast<Derived&>(*this);
  }

  ~FunctionBase() { Clear(); }

  Ret operator()(Args... args) const {
    if (object_ == nullptr) {
      throw std::bad_function_call();
    }
    return invoke_(object_, std::forward<Args>(args)...);
  }

  explicit operator bool() const { return object_ != nullptr; }

  bool operator==(std::nullptr_t) const { return object_ == nullptr; }

  template <typename F>
  F* Target() {
    return TargetType() == typeid(F) ? static_cast<F*>(object_) : nullptr;
  }

  template <typename F>
  const F* Target() const {
    return TargetType() == typeid(F) ? static_cast<const F*>(object_) : nullptr;
  }

  const std::type_info& TargetType() const {
    return object_ != nullptr ? operations_->type() : typeid(void);
  }
};

template <typename T>
struct SignatureOf;

template <typename Class, typename Ret, typename... Args>
struct SignatureOf<Ret (Class::*)(Args...)> {
  using Type = Ret(Args...);
};

template <typename Class, typename Ret, typename... Args>
struct SignatureOf<Ret (Class::*)(Args...) const> {
  using Type = Ret(Args...);
};

template <typename T>
class Function;

template <typename Ret, typename... Args>
class Function<Ret(Args...)>
    : public FunctionBase<true, Function<Ret(Args...)>, Ret(Args...)> {
 private:
  using Base = FunctionBase<true, Function<Ret(Args...)>, Ret(Args...)>;

 public:
  using Base::Base;
  using Base::operator=;

  Function() = default;
};

template <typename Ret, typename... Args>
Function(Ret (*)(Args...)) -> Function<Ret(Args...)>;

template <typename F, typename S = typename SignatureOf<decltype(&F::operator())>::Type>
Function(F) -> Function<S>;

template <typename T>
class MoveOnlyFunction;

template <typename Ret, typename... Args>
class MoveOnlyFunction<Ret(Args...)>
    : public FunctionBase<false, MoveOnlyFunction<Ret(Args...)>, Ret(Args...)> {
 private:
  using Base =
      FunctionBase<false, MoveOnlyFunction<Ret(Args...)>, Ret(Args...)>;

 public:
  using Base::Base;
  using Base::operator=;

  MoveOnlyFunction() = default;
  MoveOnlyFunction(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction(MoveOnlyFunction&&) = default;
  MoveOnlyFunction& operator=(const MoveOnlyFunction&) = delete;
  MoveOnlyFunction& operator=(MoveOnlyFunction&&) = default;
};

template <typename Ret, typename... Args>
MoveOnlyFunction(Ret (*)(Args...)) -> MoveOnlyFunction<Ret(Args...)>;

template <typename F, typename S = typename SignatureOf<decltype(&F::operator())>::Type>
MoveOnlyFunction(F) -> MoveOnlyFunction<S>;
