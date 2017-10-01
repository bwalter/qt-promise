/****************************************************************************
**
** Copyright (C) 2017 Benoit Walter
** Contact: benoit.walter@meerun.com
**
** Contains code from AsyncFuture (https://github.com/benlau/asyncfuture)
** Copyright (C) 2017 Ben Lau
**
** Licensed to the Apache Software Foundation (ASF) under one
** or more contributor license agreements.  See the NOTICE file
** distributed with this work for additional information
** regarding copyright ownership.  The ASF licenses this file
** to you under the Apache License, Version 2.0 (the
** "License"); you may not use this file except in compliance
** with the License.  You may obtain a copy of the License at
** 
**   http://www.apache.org/licenses/LICENSE-2.0
** 
** Unless required by applicable law or agreed to in writing,
** software distributed under the License is distributed on an
** "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
** KIND, either express or implied.  See the License for the
** specific language governing permissions and limitations
** under the License.
****************************************************************************/

// TODO:
// static Promise::resolved(T)
// PromiseContext(QThreadPool *)
// Promise.delay(2000)
// Promise.then(promise)
// ConcurrentPromises(FailStrategy, QThreadPool *) << promise1 << promise2 << promise3

#include <QFuture>
#include <QMetaMethod>
#include <QCoreApplication>
#include <QPointer>
#include <QThread>
#include <QFutureWatcher>
#include <QReadWriteLock>
#include <QTimer>
#include <functional>

namespace QtPromise {

template <typename T>
class Promise;

template <typename T>
class Deferred;

class PromiseError {
  public:
    explicit PromiseError(const QString& msg = "") : m_msg(msg) {}

    PromiseError(const PromiseError &) = default;
    PromiseError& operator=(const PromiseError &) = default;

    bool isContextDestroyed() const { return m_contextDestroyed; }
    const QString& message() const { return m_msg; }

    // Internal
    void _setContextDestroyed() { m_contextDestroyed = true; }

  private:
    QString m_msg;
    bool m_contextDestroyed = false;
};

namespace Private {

template <typename T>
class Value {
public:
    Value() {}
    Value(T &&v) : value(v) {}
    Value(const T &v) : value(v) {}
    Value(const T &&v) : value(v) {}
    Value(T *v) : value(*v) {}

    T value = T();
};

template <>
class Value<void> {
  public:
    Value() {}
    Value(void *) {}
};

// Based on comment in:
// http://stackoverflow.com/questions/21646467/how-to-execute-a-functor-or-a-lambda-in-a-given-thread-in-qt-gcd-style
template <typename F>
static void runInObjectThread(QObject *object, F &&func) {
  using Func = typename std::decay<F>::type;

  struct Event : public QEvent {
    Event(Func &&f) : QEvent(QEvent::None), func(std::move(f)) {}
    Event(const Func &f) : QEvent(QEvent::None), func(f) {}
    ~Event() { func(); }

    Func func;
  };

  QCoreApplication::postEvent(object, new Event(std::forward<Func>(func)));
}

struct ConnectionReceiverWrapper {
  public:
    ConnectionReceiverWrapper() = delete;

    ConnectionReceiverWrapper(QObject *o, bool autoDelete)
     : m_receiverPtr(o)
     , m_autoDelete(autoDelete) {
    }

    ~ConnectionReceiverWrapper() {
      if (m_autoDelete && !m_receiverPtr.isNull()) {
        m_receiverPtr->deleteLater();
      }
    }

    QObject *receiver() const {
      return m_receiverPtr.data();
    }

  private:
    friend class SharedContext;

    QPointer<QObject> m_receiverPtr;
    bool m_autoDelete;
};

typedef QSharedPointer<ConnectionReceiverWrapper> ConnectionReceiverWrapperPtr;

typedef enum { Idle = 0, Running, Resolved, Rejected, Canceled, ContextDestroyed } DeferObjectStatus;

template <typename T>
class DeferObject {
  public:
    typedef QSharedPointer<DeferObject<T>> Ptr;

    ~DeferObject() {
      QObject::disconnect(m_contextDestroyedConnection);
    }

    // Create instance
    template <typename ...ARGS>
    static DeferObject<T>::Ptr create(ARGS &&...args) {
      auto newInstance = new DeferObject<T>(std::forward<ARGS>(args)...);
      auto ptr = Ptr(newInstance);
      newInstance->m_thisWeakPtr = ptr;
      return ptr;
    }

    void addCallback(const Private::ConnectionReceiverWrapperPtr &receiverWrapperPtr, std::function<void()> &&func) {
      {
        QWriteLocker lock(&m_lock);

        Q_ASSERT(!receiverWrapperPtr.isNull());
        m_callbacks.append(qMakePair(receiverWrapperPtr, std::move(func)));
      }

      QReadLocker lock(&m_lock);

      if (m_status >= Resolved) {
        // Already completed
        _notify();
      }
    }

    DeferObjectStatus status() const {
      QReadLocker lock(&m_lock);
      return m_status;
    }

    const Value<T> &value() const {
      QReadLocker lock(&m_lock);
      return m_value;
    }

    const PromiseError &promiseError() const {
      QReadLocker lock(&m_lock);
      return m_promiseError;
    }

    void start() {
      QWriteLocker lock(&m_lock);

      Q_ASSERT(m_status == Idle);
      m_status = Running;

      // Do not listen any more for the context destroyed event
      QObject::disconnect(m_contextDestroyedConnection);
    }

    // Resolve without value
    void resolve() {
      {
        QWriteLocker lock(&m_lock);
        m_status = Resolved;
      }

      _notify();
    }

    void resolve(Value<void> value) {
      Q_UNUSED(value);

      {
        QWriteLocker lock(&m_lock);
        m_status = Resolved;
      }

      _notify();
    }

    // Resolve with a non-void value
    template <typename U = T,
      typename = typename std::enable_if<!std::is_same<U, void>::value>::type>
    void resolve(const U &value) {
      {
        QWriteLocker lock(&m_lock);

        m_status = Resolved;
        m_value = Value<U>(value);
      }

      _notify();
    }

    template <typename U = T,
      typename = typename std::enable_if<!std::is_same<U, void>::value>::type>
    void resolve(const Value<U> &value) {
      resolve(value.value);
    }

    // Resolve with a void promise
    template <typename U = T, typename = typename std::enable_if<std::is_same<U, void>::value>::type>
    void resolve(const Promise<void> &promise);

    // Resolve with a non-void promise
    template <typename U = T, typename = typename std::enable_if<!std::is_same<U, void>::value>::type>
    void resolve(const Promise<U> &promise);

    // Resolve with a future
    void resolve(const QFuture<T> &future) {
      if (future.isCanceled()) {
        cancel();
        return;
      }

      if (future.isFinished()) {
        resolve(future.result());
        return;
      }

      auto deleter = [](QFutureWatcher<T> *instance) {
        instance->deleteLater();
      };
      auto newFutureWatcher = new QFutureWatcher<T>();
      newFutureWatcher->setFuture(future);
      QSharedPointer<QFutureWatcher<T>> watcher(newFutureWatcher, deleter);

      auto strongThis = m_thisWeakPtr.toStrongRef();
      Q_ASSERT(!strongThis.isNull());
      auto conn1 = QObject::connect(watcher.data(), &QFutureWatcher<T>::finished, [strongThis, watcher]() {
        if (watcher->isCanceled()) {
          strongThis->cancel();
        } else {
          strongThis->resolve(watcher->result());
        }
      });
      QObject::connect(watcher.data(), &QFutureWatcher<T>::finished, [strongThis, conn1]() {
        QObject::disconnect(conn1);
      });

      watcher->setFuture(future);
    }

    void reject(const PromiseError &promiseError) {
      {
        QWriteLocker lock(&m_lock);

        m_status = Rejected;
        m_promiseError = promiseError;
      }

      _notify();
    }

    void cancel() {
      {
        QWriteLocker lock(&m_lock);
        m_status = Canceled;
      }

      _notify();
    }

  private:
    template<typename ALL>
    friend class DeferObject; // every Deferred<X> is a friend

    DeferObject() = delete;

    explicit DeferObject(QObject *owner)
     : m_lock(QReadWriteLock::Recursive) {
      if (owner) {
        // Detect deletion of the context object
        m_contextDestroyedConnection = QObject::connect(owner, &QObject::destroyed, [=] {
          QWriteLocker lock(&m_lock);
          if (m_status == Idle) {
            m_status = ContextDestroyed;
            m_promiseError._setContextDestroyed();

            lock.unlock();
            _notify();
          }
        });
      }
    }

    void _notify() {
      QReadLocker lock(&m_lock);

      if (m_callbacks.isEmpty()) {
        return;
      }

      for (const auto &callback : m_callbacks) {
        const ConnectionReceiverWrapperPtr &receiverWrapperPtr = callback.first;
        const std::function<void()> &func = callback.second;
        Q_ASSERT(!receiverWrapperPtr.isNull());
        Q_ASSERT(receiverWrapperPtr->receiver());
        if (receiverWrapperPtr->receiver()) {
          Q_ASSERT(func);
          auto thisWeakPtr = m_thisWeakPtr;
          auto safeFunc = [thisWeakPtr, func]() {
            auto thisStrong = thisWeakPtr.toStrongRef();
            if (!thisStrong.isNull()) {
              QReadLocker lock(&thisStrong->m_lock);
              func();
            }
          };
          runInObjectThread(receiverWrapperPtr->receiver(), safeFunc);
        }
      }

      m_callbacks.clear();
    }

    QWeakPointer<DeferObject<T>> m_thisWeakPtr;
    mutable QReadWriteLock m_lock;

    DeferObjectStatus m_status = Idle;
    Value<T> m_value;
    PromiseError m_promiseError;  // TODO: optional?
    QMetaObject::Connection m_contextDestroyedConnection;
    QList<QPair<ConnectionReceiverWrapperPtr, std::function<void()>>> m_callbacks;
};

// Determine the input type a QFuture
template <typename T>
struct future_traits {
  enum {
    is_future = 0
  };

 typedef void arg_type;
};

template <template <typename> class C, typename T>
struct future_traits<C <T> >
{
  enum {
    is_future = 0
  };

  typedef void arg_type;
};

template <typename T>
struct future_traits<QFuture<T> >{
  enum {
    is_future = 1
  };
  typedef T arg_type;
};


// function_traits: Source: http://stackoverflow.com/questions/7943525/is-it-possible-to-figure-out-the-parameter-type-and-return-type-of-a-lambda

template <typename T>
struct function_traits
 : public function_traits<decltype(&T::operator())>
{};

template <typename ClassType, typename ReturnType, typename ...Args>
struct function_traits<ReturnType(ClassType::*)(Args...) const>
// we specialize for pointers to member function
{
  enum { arity = sizeof...(Args) };
  // arity is the number of arguments.

  typedef ReturnType result_type;

  enum {
    result_type_is_future = future_traits<result_type>::is_future
  };

  // If the result_type is a QFuture<T>, the type will be T. Otherwise, it is void
  typedef typename future_traits<result_type>::arg_type future_arg_type;


  template <size_t i>
  struct arg {
    typedef typename std::tuple_element<i, std::tuple<Args...>>::type type;
    // the i-th argument is equivalent to the i-th tuple element of a tuple
    // composed of those arguments.
  };
};

template <typename T>
struct signal_traits {
};

template <typename R, typename C>
struct signal_traits<R (C::*)()> {
  typedef void result_type;
};

template <typename R, typename C, typename ARG1>
struct signal_traits<R (C::*)(ARG1)> {
  typedef ARG1 result_type;
};

template <typename R, typename C, typename ARG1>
struct signal_traits<R (C::*)(const ARG1 &)> {
  typedef ARG1 result_type;
};

template <typename R, typename C, typename ARG1, typename ARG2, typename ...ARGS>
struct signal_traits<R (C::*)(ARG1, ARG2, ARGS...)> {
  typedef ARG1 result_type;
};

template <typename R, typename C, typename ARG1, typename ARG2, typename ...ARGS>
struct signal_traits<R (C::*)(const ARG1 &, ARG2, ARGS...)> {
  typedef ARG1 result_type;
};

template <typename T>
struct async_traits {
  enum {
    is_future = 0,
    is_promise = 0
  };
};

template <template <typename> class C, typename T>
struct async_traits<C <T> >
{
  enum {
    is_future = 0,
    is_promise = 0
  };
};

template <typename T>
struct async_traits<QFuture<T> >{
  enum {
    is_future = 1,
    is_promise = 0
  };
  typedef T arg_type;
};

template <typename T>
struct async_traits<Promise<T> >{
  enum {
    is_future = 0,
    is_promise = 1
  };
  typedef T arg_type;
};

template <typename Functor, typename ArgType>
typename std::enable_if<
  Private::function_traits<Functor>::arity == 1,
  Value<typename Private::function_traits<Functor>::result_type>
>::type
invoke(Functor &&functor, const Value<ArgType> &argValue) {
  Value<typename Private::function_traits<Functor>::result_type> value(std::move(functor)(argValue.value));
  return value;
}

template <typename Functor, typename ArgType>
typename std::enable_if<
  Private::function_traits<Functor>::arity != 1,
  Value<typename Private::function_traits<Functor>::result_type>
>::type
invoke(Functor &&functor, const Value<ArgType> &argValue) {
  Q_UNUSED(argValue);
  static_assert(Private::function_traits<Functor>::arity == 0, "Your callback should not take any argument because the observed type is QFuture<void>");
  Value<typename Private::function_traits<Functor>::result_type> value(std::move(functor)());
  return value;
}

template <typename Functor, typename ArgType>
typename std::enable_if<
  (Private::function_traits<Functor>::arity != 1 || std::is_same<ArgType, void>::value),
  void
>::type
voidInvoke(Functor functor, const Value<ArgType> &argValue) {
  Q_UNUSED(argValue);
  functor();
}

template <typename Functor, typename ArgType>
typename std::enable_if<
  (Private::function_traits<Functor>::arity == 1 && !std::is_same<ArgType, void>::value),
  void
>::type
voidInvoke(Functor functor, const Value<ArgType> &argValue) {
  functor(argValue.value);
}

template <typename Functor, typename ArgType>
typename std::enable_if<
  !std::is_same<typename Private::function_traits<Functor>::result_type, void>::value,
  Value<typename Private::function_traits<Functor>::result_type>
>::type
run(Functor &&functor, const Value<ArgType> &argValue) {
  auto value = invoke(std::move(functor), argValue);
  return value;
}

template <typename Functor, typename ArgType>
typename std::enable_if<std::is_same<typename Private::function_traits<Functor>::result_type, void>::value,
Value<void>>::type
run(Functor &&functor, const Value<ArgType> &argValue) {
  voidInvoke(std::move(functor), argValue);
  return Value<void>();
}

class SharedContext {
  public:
    explicit SharedContext(QObject *co)
     : m_contextObject(co) {
      _updateConnectionReceiver();
    }

    QObject *contextObject() const { return m_contextObject.data(); }
    ConnectionReceiverWrapperPtr connectionReceiverWrapper() const { return m_connectionReceiverWrapperPtr; }

    void setContextObject(QObject *o) {
      m_contextObject = o;
      _updateConnectionReceiver();
    }
 
    void resetContextObject() {
      m_contextObject = nullptr;
      _updateConnectionReceiver();
    }

  private:
    void _updateConnectionReceiver() {
      QThread *thread;
      if (m_contextObject.isNull()) {
        // No context given -> use current thread
        thread = QThread::currentThread();
      } else if (qobject_cast<QThread *>(m_contextObject.data())) {
        // Thread explicitly given as context
        thread = static_cast<QThread *>(m_contextObject.data());
      } else {
        // Use thread of object given as context
        thread = m_contextObject->thread();
      }

      if (thread == qApp->thread()) {
        // The application can be used without creating a temporary object
        m_connectionReceiverWrapperPtr = QSharedPointer<ConnectionReceiverWrapper>::create(qApp, false /*autoDelete*/);
        return;
      }

      // Create an object living in the thread
      QObject *tmpObject = new QObject();
      tmpObject->moveToThread(thread);
      m_connectionReceiverWrapperPtr = QSharedPointer<ConnectionReceiverWrapper>::create(tmpObject, true /*autoDelete*/);
    }

    QPointer<QObject> m_contextObject;
    ConnectionReceiverWrapperPtr m_connectionReceiverWrapperPtr;
};

typedef QSharedPointer<SharedContext> SharedContextPtr;

// Helper for T != void
template <typename T>
struct DeferredConnectHelper {
  template <typename Emitter, typename Member>
  static void connectAndResolve(Deferred<T> *that, Emitter emitter, Member pointerToMemberFunction);
};

// Helper for T = void
template <>
struct DeferredConnectHelper<void> {
  template <typename Emitter, typename Member>
  static void connectAndResolve(Deferred<void> *that, Emitter emitter, Member pointerToMemberFunction);
};

} // End of Private Namespace

template <typename T = void>
class Promise {
  public:
    // Successfull (void) promise
    template <typename U = T,
      typename = typename std::enable_if<std::is_same<U, void>::value>::type>
    Promise()
     : m_deferObject(Private::DeferObject<U>::create(nullptr)) {
      m_deferObject->resolve();
    }

    // Successfull promise with value
    template <typename U = T,
      typename = typename std::enable_if<!std::is_same<U, void>::value>::type>
    Promise(const U &value)
     : m_deferObject(Private::DeferObject<T>::create(nullptr)) {
      m_deferObject->resolve(T(value));
    }

    // TODO: Promise with a future
    template <typename FutureType>
    Promise(const QFuture<FutureType> &) {
      Q_ASSERT(false);
    }

    // TODO: explicitly remove context?
    Promise(const Promise &other) = default;
    Promise(Promise &&other) = default;
    Promise &operator=(const Promise &other) = default;

    virtual ~Promise() {}

    bool isPending() const {
      return m_deferObject->status() <= Private::DeferObjectStatus::Running;
    }

    bool isFinished() const {
      // TODO: is canceled considered to be finished?
      return m_deferObject->status() > Private::DeferObjectStatus::Running;
    }

    bool isFulfilled() const {
      return m_deferObject->status() == Private::DeferObjectStatus::Resolved;
    }

    bool hasError() const {
      // TODO: is canceled an error?
      return m_deferObject->status() == Private::DeferObjectStatus::Rejected  ||
        m_deferObject->status() == Private::DeferObjectStatus::ContextDestroyed;
    }

    bool isCanceled() const {
      return m_deferObject->status() == Private::DeferObjectStatus::Canceled;
    }

    PromiseError error() const {
      return m_deferObject->promiseError();
    }

    QObject *contextObject() const {
      return m_contextObject;
    }

    // Return a non-void value
    template <typename U = T,
      typename = typename std::enable_if<!std::is_same<U, void>::value>::type>
    //TODO: const T &value() const { return m_deferObject->value(); }
    T value() const { return m_deferObject->value().value; }

    // With function returning wether a QFuture nor a Promise
    template <typename Functor>
    typename std::enable_if<
      !Private::async_traits<typename Private::function_traits<Functor>::result_type>::is_future
      && !Private::async_traits<typename Private::function_traits<Functor>::result_type>::is_promise,
    Promise<typename Private::function_traits<Functor>::result_type>
    >::type
    then(Functor &&functor) const {
      return _then<typename Private::function_traits<Functor>::result_type,
                   typename Private::function_traits<Functor>::result_type
                  >(std::move(functor));
    }

    // With function returning a QFuture
    template <typename Functor>
    typename std::enable_if<Private::async_traits<typename Private::function_traits<Functor>::result_type>::is_future,
      Promise<typename Private::async_traits<typename Private::function_traits<Functor>::result_type>::arg_type>
    >::type
    then(Functor &&functor) const {
      return _then<typename Private::async_traits<typename Private::function_traits<Functor>::result_type>::arg_type,
                   typename Private::function_traits<Functor>::result_type
                  >(std::move(functor));
    }

    // With function returning a Promise
    template <typename Functor>
    typename std::enable_if< Private::async_traits<typename Private::function_traits<Functor>::result_type>::is_promise,
    Promise<typename Private::async_traits<typename Private::function_traits<Functor>::result_type>::arg_type>
    >::type
    then(Functor &&functor) const {
      return _then<typename Private::async_traits<typename Private::function_traits<Functor>::result_type>::arg_type,
                   typename Private::function_traits<Functor>::result_type
                  >(std::move(functor));
    }

    template <typename Functor>
    Promise<T>
    tap(Functor &&functor) const {
      Promise<T> p = *this;
      return _then<typename Private::function_traits<Functor>::result_type,
                   typename Private::function_traits<Functor>::result_type
                  >(std::move(functor))
      .then([=]() {
        return p;
      });
    }

    Promise<T> delay(int ms) const {
      auto next = *this;
      return then([next, ms](){
        auto deferPtr = QSharedPointer<Deferred<T>>(new Deferred<T>());
        auto t = new QTimer(next.contextObject());

        QObject::connect(t, &QTimer::timeout, [=](){
          deferPtr->resolve(next);
        });

        QObject::connect(t, &QTimer::timeout, t, &QObject::deleteLater);
        t->start(ms);
        return deferPtr->promise();
      });
    }

    // TODO: With a promise
#if 0
    template <typename PromiseType>
    Promise<PromiseType> then(const Promise<PromiseType> &otherPromise) {
      auto func = [=]() {
        return otherPromise;
      };

      return _then(func);
    }
#endif

    // Execute the given function after a rejected promise and
    // return a new promise object with unchanged result
    Promise<T>
    fail(std::function<void()> &&fail) const {
      return _fail<T, void>(std::move(fail));
    }

    // Execute the given function after a rejected promise and return
    // a new promise
    // TODO:
#if 0
    template <typename PromiseType>
    Promise<PromiseType> catch(std::function<Promise<PromiseType>()> fail) const {
      return _fail<T, void>(fail);
    }
#endif

    // Execute the given function after a rejected promise and
    // return a new promise object with unchanged result
    Promise<T>
    fail(std::function<void(const PromiseError&)> &&fail) const {
      return _fail<T, void>(std::move(fail));
    }

    Promise<T>
    finally(std::function<void()> &&func) const {
      return _finally<T, void>(std::move(func));
    }

    Promise<T>
    finally(std::function<void(QObject *)> &&func) const {
      return _finally<T, void>(std::move(func));
    }

  private:
    enum PromiseCallType { ThenCall = 0, FailCall, FinallyCall };

    Promise(QSharedPointer<Private::DeferObject<T>> &&deferObject)
     : m_deferObject(std::move(deferObject)) {
    }

    Promise(const QSharedPointer<Private::SharedContext> &context)
     : m_contextObject(context->contextObject())
     , m_sharedContext(context)
     , m_deferObject(Private::DeferObject<T>::create(m_sharedContext->contextObject())) {}

    Promise(QSharedPointer<Private::SharedContext> &&context)
     : m_contextObject(context->contextObject())
     , m_sharedContext(std::move(context))
     , m_deferObject(Private::DeferObject<T>::create(m_sharedContext->contextObject())) {}

    Promise(const QSharedPointer<Private::SharedContext> &context,
            const QSharedPointer<Private::DeferObject<T>> &deferObject)
     : m_contextObject(context->contextObject())
     , m_sharedContext(context)
     , m_deferObject(deferObject) {}

    template <typename PromiseType, typename RetValType, typename Functor>
    Promise<PromiseType> _then(Functor &&functor) const {
      static_assert(Private::function_traits<Functor>::arity <= 1, "_then(): Callback should take not more than one parameter");

      auto deferObject = m_deferObject;
      auto nextDeferObject = Private::DeferObject<PromiseType>::create(m_sharedContext->contextObject());

      Q_ASSERT(deferObject.data());

      auto connectionReceiverWrapperPtr = m_sharedContext->connectionReceiverWrapper();

      deferObject->addCallback(connectionReceiverWrapperPtr, [=]() {
        Q_ASSERT(!deferObject.isNull());
        if (nextDeferObject->status() == Private::DeferObjectStatus::ContextDestroyed) {
          return;
        }

        // Current promise completed!
        switch (deferObject->status()) {
          case Private::DeferObjectStatus::Idle:
          case Private::DeferObjectStatus::Running:
            Q_UNREACHABLE();

          case Private::DeferObjectStatus::Resolved:
            try {
              nextDeferObject->start();
              Private::Value<RetValType> value = Private::run(std::move(functor), deferObject->value());

              // "then" reflects the return value of the functor
              nextDeferObject->resolve(value);
            }
            catch (const PromiseError &error) {
              // An error thrown inside the functor is propagated for both "then" and "finally"
              nextDeferObject->reject(error);
            }
            break;

          case Private::DeferObjectStatus::Rejected:
          case Private::DeferObjectStatus::ContextDestroyed:
            // "then" function is not executed when the promise has been rejected
            // and the promise error is simply forwarded
            nextDeferObject->reject(deferObject->promiseError());
            break;

          case Private::DeferObjectStatus::Canceled:
            // Current promise canceled => cancel next one, too
            nextDeferObject->cancel();
            break;
        }
      });

      return Promise<PromiseType>(m_sharedContext, nextDeferObject);
    }

    template <typename PromiseType, typename RetValType, typename Functor>
    Promise<PromiseType> _fail(Functor &&functor) const {
      static_assert(Private::function_traits<Functor>::arity <= 1, "_then(): Callback should take not more than one parameter");

      auto deferObject = m_deferObject;
      auto nextDeferObject = Private::DeferObject<PromiseType>::create(nullptr);

      Q_ASSERT(deferObject.data());

      auto connectionReceiverWrapperPtr = m_sharedContext->connectionReceiverWrapper();

      deferObject->addCallback(connectionReceiverWrapperPtr, [=]() {
        Q_ASSERT(!deferObject.isNull());

        // Current promise completed!
        switch (deferObject->status()) {
          case Private::DeferObjectStatus::Idle:
          case Private::DeferObjectStatus::Running:
            Q_UNREACHABLE();

          case Private::DeferObjectStatus::Resolved:
            // "fail" function is not executed when the promise has been resolved
            // and the promise value is simply forwarded
            nextDeferObject->resolve(deferObject->value());
            break;

          case Private::DeferObjectStatus::Rejected:
          case Private::DeferObjectStatus::ContextDestroyed:
            try {
              nextDeferObject->start();
              Private::run(std::move(functor), Private::Value<PromiseError>(deferObject->promiseError()));
              nextDeferObject->reject(deferObject->promiseError());
            }
            catch (const PromiseError &error) {
              // An error thrown inside the functor is propagated for both "fail" and "finally"
              nextDeferObject->reject(error);
            }
            break;

          case Private::DeferObjectStatus::Canceled:
            // Current promise canceled => cancel next one, too
            nextDeferObject->cancel();
            break;
        }
      });

      return Promise<PromiseType>(m_sharedContext, nextDeferObject);
    }

    template <typename PromiseType, typename RetValType, typename Functor>
    Promise<PromiseType> _finally(Functor &&functor) const {
      static_assert(Private::function_traits<Functor>::arity <= 1, "_then(): Callback should take not more than one parameter");

      auto deferObject = m_deferObject;
      auto nextDeferObject = Private::DeferObject<PromiseType>::create(nullptr);

      Q_ASSERT(deferObject.data());

      auto connectionReceiverWrapperPtr = m_sharedContext->connectionReceiverWrapper();
      QPointer<QObject> contextObjectPtr(m_sharedContext->contextObject());

      deferObject->addCallback(connectionReceiverWrapperPtr, [=]() {
        Q_ASSERT(!deferObject.isNull());

        // Current promise completed!
        switch (deferObject->status()) {
          case Private::DeferObjectStatus::Idle:
          case Private::DeferObjectStatus::Running:
            Q_UNREACHABLE();

          case Private::DeferObjectStatus::Resolved:
            try {
              nextDeferObject->start();
              Private::run(std::move(functor), Private::Value<QObject *>(contextObjectPtr.data()));

              // "finally" forwards the value of the promise (not the return value of the functor)
              nextDeferObject->resolve(deferObject->value());
            }
            catch (const PromiseError &error) {
              // An error thrown inside the functor is propagated for both "then" and "finally"
              nextDeferObject->reject(error);
            }
            break;

          case Private::DeferObjectStatus::Rejected:
          case Private::DeferObjectStatus::ContextDestroyed:
            try {
              nextDeferObject->start();
              Private::run(std::move(functor), Private::Value<QObject *>(contextObjectPtr.data()));
              nextDeferObject->reject(deferObject->promiseError());
            }
            catch (const PromiseError &error) {
              // An error thrown inside the functor is propagated for both "fail" and "finally"
              nextDeferObject->reject(error);
            }
            break;

          case Private::DeferObjectStatus::Canceled:
            // Current promise canceled => cancel next one, too
            nextDeferObject->cancel();
            break;
        }
      });

      return Promise<PromiseType>(m_sharedContext, nextDeferObject);
    }

  private:
    template<typename ALL>
    friend class Promise;

    template<typename ALL>
    friend class Deferred;

    template<typename ALL>
    friend struct Private::DeferredConnectHelper;

    template<typename ALL>
    friend class Private::DeferObject;

    friend class PromiseContext;

    QObject *m_contextObject = nullptr;
    QSharedPointer<Private::SharedContext> m_sharedContext = QSharedPointer<Private::SharedContext>::create(nullptr);
    typename Private::DeferObject<T>::Ptr m_deferObject;
};

class PromiseContext : private Promise<void> {
  public:
    explicit PromiseContext(QObject *object)
     : Promise<void>(QSharedPointer<Private::SharedContext>::create(object)) {
      Promise<void>::m_deferObject->resolve();
    }

    ~PromiseContext() override {
      Promise<void>::m_sharedContext->resetContextObject();
    }

    using Promise<void>::then;
    //using Promise<void>::connect;

  private:
    PromiseContext(const PromiseContext &) = delete;
    PromiseContext(PromiseContext &&) = delete;
    PromiseContext& operator=(const PromiseContext &) = delete;
};

template <typename T = void>
class Deferred {
  public:
    // TODO: with context?
    Deferred()
     : m_promise(Private::DeferObject<T>::create(nullptr)) {
    }

    const Promise<T> &promise() const {
      return m_promise;
    }

    // Resolve a void promise
    void resolve() {
      m_promise.m_deferObject->resolve();
    }

    // Resolve a non-void promise
    template <typename U = T, typename = typename std::enable_if<!std::is_same<U, void>::value>::type>
    void resolve(const U &value) {
      m_promise.m_deferObject->resolve(value);
    }

    // TODO: Resolve with a future?
#if 0
    void complete(const QFuture<T> &future) {
    }
#endif

    // TODO: Resolve with a promise?
#if 0
    void complete(const Promise<T> &promise) {
    }
#endif

    void reject() {
      m_promise.m_deferObject->reject(PromiseError());
    }

    void reject(const PromiseError &error) {
      m_promise.m_deferObject->reject(error);
    }

    template <
      typename Emitter,
      typename Member,
      typename Func,
      typename ...ARGS
    >
    void connect(Emitter emitter, Member pointerToMemberFunction, const Func &func) {
      auto deferObjectPtr = m_promise.m_deferObject;
      auto deferObjectWeakPtr = QWeakPointer<Private::DeferObject<T>>(m_promise.m_deferObject);

      auto safeFunc = [deferObjectWeakPtr, emitter, func](auto &&...args) {
        auto deferObjectPtr = deferObjectWeakPtr.toStrongRef();
        if (deferObjectPtr.isNull() || deferObjectPtr->status() >= Private::DeferObjectStatus::Resolved) {
          return;
        }

        func(args...);
      };

      auto conn = QObject::connect(emitter, pointerToMemberFunction, safeFunc);

      // Disconnect when resolved
      auto receiverWrapperPtr = Private::ConnectionReceiverWrapperPtr::create(emitter, false /*autoDeletE*/);
      m_promise.m_deferObject->addCallback(receiverWrapperPtr, [conn]() {
        QObject::disconnect(conn);
      });
    }

    template <typename Emitter, typename Member>
    void connectAndResolve(Emitter emitter, Member pointerToMemberFunction) {
      Private::DeferredConnectHelper<T>::connectAndResolve(this, emitter, pointerToMemberFunction);
    }

    template <
      typename U = T,
      typename Emitter,
      typename Member,
      typename = typename std::enable_if<!std::is_same<U, void>::value>::type
    >
    void connectAndResolve(Emitter emitter, Member pointerToMemberFunction, const U &value) {
      auto deferObjectPtr = m_promise.m_deferObject;
      auto deferObjectWeakPtr = QWeakPointer<Private::DeferObject<T>>(m_promise.m_deferObject);

      auto conn = QObject::connect(emitter, pointerToMemberFunction, [=]() {
        auto deferObjectPtr = deferObjectWeakPtr.toStrongRef();
        if (deferObjectPtr.isNull() || deferObjectPtr->status() >= Private::DeferObjectStatus::Resolved) {
          return;
        }

        deferObjectPtr->resolve(Private::Value<T>(value));
      });

      // Disconnect afterwards
      QObject::connect(emitter, pointerToMemberFunction, [=]() {
        QObject::disconnect(conn);
      });
    }

    template <typename Emitter, typename Member>
    void connectAndReject(Emitter emitter, Member pointerToMemberFunction, const PromiseError &error = PromiseError()) {
      auto deferObjectPtr = m_promise.m_deferObject;
      auto conn = QObject::connect(emitter, pointerToMemberFunction, [=]() {
        deferObjectPtr->reject(error);
      });
      QObject::connect(emitter, pointerToMemberFunction, [=]() {
        QObject::disconnect(conn);
      });
    }

  private:
    friend struct Private::DeferredConnectHelper<T>;

    Promise<T> m_promise;
};

// static
template <typename T>
template <typename Emitter, typename Member>
void Private::DeferredConnectHelper<T>::connectAndResolve(Deferred<T> *that, Emitter emitter, Member pointerToMemberFunction) {
  auto deferObjectPtr = that->m_promise.m_deferObject;
  auto deferObjectWeakPtr = QWeakPointer<Private::DeferObject<T>>(that->m_promise.m_deferObject);

  auto conn = QObject::connect(emitter, pointerToMemberFunction, [=](const T &arg) {
    auto deferObjectPtr = deferObjectWeakPtr.toStrongRef();
    if (deferObjectPtr.isNull() || deferObjectPtr->status() >= Private::DeferObjectStatus::Resolved) {
      return;
    }

    deferObjectPtr->resolve(Private::Value<T>(arg));
  });

  // Disconnect afterwards
  QObject::connect(emitter, pointerToMemberFunction, [=]() {
    QObject::disconnect(conn);
  });
}

// static
template <typename Emitter, typename Member>
void Private::DeferredConnectHelper<void>::connectAndResolve(Deferred<void> *that, Emitter emitter, Member pointerToMemberFunction) {
  auto deferObjectPtr = that->m_promise.m_deferObject;
  auto deferObjectWeakPtr = QWeakPointer<Private::DeferObject<void>>(that->m_promise.m_deferObject);

  auto conn = QObject::connect(emitter, pointerToMemberFunction, [=]() {
    auto deferObjectPtr = deferObjectWeakPtr.toStrongRef();
    if (deferObjectPtr.isNull() || deferObjectPtr->status() >= Private::DeferObjectStatus::Resolved) {
      return;
    }

    deferObjectPtr->resolve();
  });

  // Disconnect afterwards
  QObject::connect(emitter, pointerToMemberFunction, [=]() {
    QObject::disconnect(conn);
  });
}

template <typename T>
template <typename U, typename>
void Private::DeferObject<T>::resolve(const Promise<void> &promise) {
  auto otherDeferObject = promise.m_deferObject;
  Q_ASSERT(!otherDeferObject.isNull());

  auto strongThis = m_thisWeakPtr.toStrongRef();
  Q_ASSERT(!strongThis.isNull());

  promise
  .then([strongThis]() {
    strongThis->resolve();
  })
  .fail([strongThis](const PromiseError &error) {
    strongThis->reject(error);
  });
}

template <typename T>
template <typename U, typename>
void Private::DeferObject<T>::resolve(const Promise<U> &promise) {
  auto otherDeferObject = promise.m_deferObject;
  Q_ASSERT(!otherDeferObject.isNull());

  auto strongThis = m_thisWeakPtr.toStrongRef();
  Q_ASSERT(!strongThis.isNull());

  promise
  .then([strongThis](const T &value) {
    strongThis->resolve(value);
  })
  .fail([strongThis](const PromiseError &error) {
    strongThis->reject(error);
  });
}

template <typename PromiseType, typename PromiseFunc>
Promise<PromiseType> makePromise(PromiseFunc promiseFunc) {
  auto deferPtr = QSharedPointer<Deferred<PromiseType>>(new Deferred<PromiseType>());
  auto resolveFunc = [=](const PromiseType &value) {
    deferPtr->resolve(value);
  };
  auto rejectFunc = [=](const PromiseError &error) {
    deferPtr->reject(error);
  };
  promiseFunc(resolveFunc, rejectFunc);
  return deferPtr->promise();
}

template <
  typename Emitter,
  typename Member,
  typename = typename std::enable_if<
    !std::is_same<typename Private::signal_traits<Member>::result_type, void>::value
  >::type
>
Promise<typename Private::signal_traits<Member>::result_type>
makeConnectionPromise(Emitter emitter, Member pointerToMemberFunction) {
  typedef typename Private::signal_traits<Member>::result_type RetType;

  auto deferPtr = QSharedPointer<Deferred<RetType>>(new Deferred<RetType>());

  auto conn = QObject::connect(emitter, pointerToMemberFunction, [=](const RetType &arg) {
    deferPtr->resolve(arg);
  });
  QObject::connect(emitter, pointerToMemberFunction, [=]() {
    QObject::disconnect(conn);
  });

  return deferPtr->promise();
}

template <
  typename Emitter,
  typename Member,
  typename = typename std::enable_if<
    std::is_same<typename Private::signal_traits<Member>::result_type, void>::value
  >::type
>
Promise<void>
makeConnectionPromise(Emitter emitter, Member pointerToMemberFunction) {
  auto deferPtr = QSharedPointer<Deferred<void>>(new Deferred<void>());

  auto conn = QObject::connect(emitter, pointerToMemberFunction, [=]() {
    deferPtr->resolve();
  });
  QObject::connect(emitter, pointerToMemberFunction, [=]() {
    QObject::disconnect(conn);
  });

  return deferPtr->promise();
}

}  // namespace QtPromise

