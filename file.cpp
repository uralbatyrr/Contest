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
    void (*move)(void*, void*);
    void (*copy)(void*, void*);
    const std::type_info& (*type)();
  };

  template <typename F>
  static F* Object(void* buffer) {
    if constexpr (kIsSmall<F>) {
      return static_cast<F*>(buffer);
    } else {
      return *static_cast<F**>(buffer);
    }
  }

  template <typename F, typename G>
  static void Construct(void* buffer, G&& value) {
    if constexpr (kIsSmall<F>) {
      new (buffer) F(std::forward<G>(value));
    } else {
      new (buffer) F*(new F(std::forward<G>(value)));
    }
  }

  template <typename F>
  static Ret Invoke(void* buffer, Args... args) {
    return static_cast<Ret>(
        std::invoke(*Object<F>(buffer), std::forward<Args>(args)...));
  }

  template <typename F>
  static void Destroy(void* buffer) {
    if constexpr (kIsSmall<F>) {
      Object<F>(buffer)->~F();
    } else {
      delete Object<F>(buffer);
    }
  }

  template <typename F>
  static void Move(void* source, void* buffer) {
    if constexpr (kIsSmall<F>) {
      new (buffer) F(std::move(*Object<F>(source)));
      Object<F>(source)->~F();
    } else {
      new (buffer) F*(Object<F>(source));
    }
  }

  template <typename F>
  static void Copy(void* source, void* buffer) {
    if constexpr (Copyable) {
      Construct<F>(buffer, *Object<F>(source));
    }
  }

  template <typename F>
  static const std::type_info& Type() {
    return typeid(F);
  }

  template <typename F>
  static constexpr Operations kOperations = {&Destroy<F>, &Move<F>, &Copy<F>,
                                             &Type<F>};

  alignas(std::max_align_t) mutable char buffer_[kBufferSize];
  Ret (*invoke_)(void*, Args...) = nullptr;
  const Operations* operations_ = nullptr;

  void Reset() {
    invoke_ = nullptr;
    operations_ = nullptr;
  }

  void Clear() {
    if (invoke_ != nullptr) {
      operations_->destroy(buffer_);
    }
    Reset();
  }

  void Adopt(FunctionBase&& other) {
    if (other.invoke_ != nullptr) {
      other.operations_->move(other.buffer_, buffer_);
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
    Construct<Stored>(buffer_, std::forward<F>(function));
    invoke_ = &Invoke<Stored>;
    operations_ = &kOperations<Stored>;
  }

  FunctionBase(const FunctionBase& other)
      : invoke_(other.invoke_), operations_(other.operations_) {
    if (invoke_ != nullptr) {
      operations_->copy(other.buffer_, buffer_);
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
    if (invoke_ == nullptr) {
      throw std::bad_function_call();
    }
    return invoke_(buffer_, std::forward<Args>(args)...);
  }

  explicit operator bool() const { return invoke_ != nullptr; }

  bool operator==(std::nullptr_t) const { return invoke_ == nullptr; }

  template <typename F>
  F* Target() {
    return TargetType() == typeid(F) ? Object<F>(buffer_) : nullptr;
  }

  template <typename F>
  const F* Target() const {
    return TargetType() == typeid(F) ? Object<F>(buffer_) : nullptr;
  }

  const std::type_info& TargetType() const {
    return invoke_ != nullptr ? operations_->type() : typeid(void);
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
