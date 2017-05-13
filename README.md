## QtPromise - Chainable promises for Qt

Introduction
========

Why promises?
- Promises make asynchronous operations easier to write
- No code fragmentation (compared to separate "slot" methods or nested callbacks)
- More efficient code by encouraging developers to implement asynchronous operations
  without additional complexity
- Error handling
- Safer code with a clear scope and context variables

Why QtPromise?
- Easy to use API for promises
- Integration with Qt event loops
- Define a context for variables used within a promise chain
- Limit the lifetime of the promise (e.g. stop when a given QObject has been destroyed)
- Support for QObject connect
- Support for QThread/QThreadPool
- Compatibility with QtConcurrent/QFuture


Features
========

**1. Basic concept**

A promise has:
- a status (pending, fulfilled, failed, canceled)
- a value (set when it has been fulfilled)
- the possibility to be chained with another promise ('then')

*How to create promises?*

Create a fulfilled promise with a value:
```c++
auto promise1 = Promise<void>();
auto promise2 = Promise<bool>(true);
auto promise3 = Promise<QString>("stringValue");
```

Chain an existing promise with 'Promise::then()':
```c++
auto promise = Promise<void>().then([]() {
  // resolve promise:
  return value;

  // or reject promise:
  throw(PromiseError(msg));
});
```

Call 'makePromise()' (c++14 only!):
```c++
auto promise = makePromise<T>([](auto resolve, auto reject) {
  // resolve promise...
  resolve(value);

  // ...or reject promise
  reject(PromiseError(msg));
});
```

Use a 'Deferred':
```c++
Deferred<T> defer;
auto promise = defer.promise();

// resolve promise...
defer.resolve(value);

// ...or reject promise:
defer.reject(PromiseError(msg));
});
```

**2. Promise chains**

Promises are chained using the 'Promise::then()' method. After the current promise has been resolved, the
lambda is called with the promise value as (optional) parameter and a new promise is returned.

By default, the promise chain is running in the Qt event loop of the current thread and the successive
operations are queued.

Example of promise chain without error:
```c++
Promise<int> promise = Promise<void>()
.then([]() {
  // executed in the next step of the current event loop
  // resolved without value
})
.then([]() {
  // executed in the next step of the current event loop
  return 12;
})
.fail([](const PromiseError &error) {
  // not executed because there was no error
  // Note: the returned promise is still fulfilled with the above value
})
.finally([]() {
  // always executed
  // Note: the returned promise is still fulfilled with the above value
})

promise
.then([](int value) {
  // Executed with value == 12
});
```

Example of promise chain with an error:
```c++
Promise<int> promise = Promise<void>()
.then([]() {
  // executed in the next step of the current event loop
  throw PromiseError("This is an error");
})
.then([]() {
  // not reached because of the previous error
  return 12;
})
.fail([](const PromiseError &error) {
  // executed because of the above error
  // (error.message() == "This is an error")
  // Note: the returned promise is still failed with the above error
  // unless we return here a new Promise
})
.finally([]() {
  // always executed
  // Note: the returned promise is still failed with the above error
})

promise
.then([](int value) {
  // not executed because the previous promise has failed
})
.then([](const PromiseError &error) {
  // executed because the previous promise failed
  // (error.message() == "This is an error")
});
```

**3. Errors**

A promise has an error if:
- it is chained with a promise which has an error
- an exception has been thrown with a PromiseError inside the lambda parameter of 'then()'
- it has been explicitly rejected inside 'makePromise()' or using a 'Deferred'
- the context object has been destroyed before starting the promise operation

Within a promise chain, errors are propagated to the next 'fail()'s and the 'finally' lambda
will be eventually called.

An error consists of the following information:
- message (QString)
- id (int, optional)
- data (QVariant, optional)
- isContextDestroyed (bool, automatically set when the error results from context destruction)

**4. Promise context**

A context may be defined for a promise chain by starting a chain with a 'PromiseContext' instance.

A promise context is especially useful:
- to restrict the lifetime of the promise to the one of a QObject instance (e.g. 'this')
- as a container for the variables used within a promise chain
- to define which thread/event loop is used for the promise chain

*Context scope*

The context is applied to all chained promises created within the scope of the PromiseContext instance.
This means that the promises which have been chained after the PromiseContext instance has been
deleted will not use that context.

Example:
```c++
auto promise = PromiseContext(ctx).then(...).then(...);  // context applied
promise.then(...);  // context not applied
```

Not recommended (but valid code):
```c++
Promise<void> promise;
{
  auto context = PromiseContext(ctx);
  promise = promise.then(...);  // context not applied
  promise = context.then(...);  // context applied
  promise = promise.then(...);  // context applied, too
}
promise.then(...);  // context not applied
```

*QObject as promise context*

When giving a QObject as context, we can:
- limit the lifetime of the promise chain: the promise chain is interrupted when ctx has been destroyed
- use the context as container for the variables used inside the promise chain
- use the context as parent of QObject instances created and used inside the promise chain
- trigger lambdas in the event loop of the context object thread

Note: on destruction of the context, the next promises operations ('then()') are skipped and the 'fail()' and
'finally()' lambdas are called.

Example:
```c++
struct ContextObject : QObject {
  int contextVariable = -1;
};
auto ctx = new ContextObject();

auto promise = PromiseContext(ctx)
.then([=]() {
  ctx->contextVariable = 1;
})
.then([]() {
  // Skipped if the context object has been destroyed
})
.fail([](const PromiseError &error) {
  if (error.isContextDestroyed()) {
    // when the error has been caused by the destruction of the context object
  }
})
.finally([](QObject *ctx) {
  // Note: ctx == nullptr if the context object has been destroyed
});

promise.then([]() {
  // No context anymore (even if the context object still exists)
});
```

*QThread as promise context*

Alternatively, you can give a QThread or a QThreadPool to explicitly trigger the promise execution
(each step of the promise) in the event loop of a specific thread.

Example:
```c++
QThread *thread = new QThread();
thread->start();  // start thread's event loop

auto promise = PromiseContext(thread)
.then([]() {
  // Running in the thread
});
```

**3. Defers**

Defers can be either implicitely created using 'makePromise()' or with an explicit 'Deferred' instance.

Using makePromise() (c++14 only!):
```c++
auto promise = makePromise<int>([](auto resolve, auto reject) {
  if (ok) {
    resolve(10);
  } else {
    reject(PromiseError(...));
  }
});
```

Using Deferred:
```c++
Deferred<int> defer;
auto promise = defer.promise();

if (ok) {
  defer.resolve(10);
} else {
  defer.reject(PromiseError(...));
}
```

**4. Connect object signals**

*Using makeConnectionPromise*

You can create a Promise which is automatically resolved as soon as the signal of a given object has been
emitted.

Note1: the type of the promise depends on the first parameter of the signal.

Example:
```c++
auto timer = new QTimer();
timer->start(3000);
makeConnectionPromise(timer, &QTimer::timeout)
.then([=]() {
  // 3 seconds later...
  delete timer;
  return makeConnectionPromise(emitter, &MyClass::intSignal);
})
.then([=](int value) {
  // Reached when intSignal has been emitted for the emitter instance with 'value' as parameter
});
```

*Using Deferred*

For more complex operations, a Deferred can be used. This can be done do define custom actions
after signal emittion and to trigger error.

Example:
```c++
auto downloadManager = new DownloadManager();
downloadManager->download("https://www.url.com/file");

Deferred<QByteArray> defer;

defer.connect(emitter, &MyClass::progress, [](double progress) {
  qDebug() << "Progress:" << (int)(progress * 100.0) << "%";
});
defer.connectAndResolve(emitter, &MyClass::downloadSuccessful);
defer.connectAndReject(emitter, &MyClass::downloadError, PromiseError("Download error"));

defer.promise()
.then([](const QByteArray &data) {
  qDebug() << "Successfully downloaded" << data.count() << "bytes!";
})
.fail([](const PromiseError &error) {
  qDebug() << "Failed:" << error.message();
})
.finally([=]() {
  delete downloadedManager;
});
```


**5. Integration with QtConcurrent/QFuture**

A promise can be defined based on a QFuture, which makes it easy to keep compatible with
QtConcurrent.

Example:
```c++
Promise<void>()
.then([]() {
  auto future = QtConcurrent::run([]() {
    // Code running in a thread of the current thread pool
    return 12;
  });
  return future;
.then([](int value) {
  // value == 12
});
```

**6. Concurrent/combined promises**

TODO


Example
========

```c++
using namespace QtPromise;

// Context object with data used inside the promise chain and whose lifetime
// depends on 'this' instance
struct ContextObject : QObject {
  QNetworkReply *reply = nullptr;
  qint64 bytesReceived = -1LL;
};
auto ctx = new ContextObject();
ctx->setParent(this);

Promise<QByteArray> downloadPromise = PromiseContext(ctx)
.then([]() {
  qDebug() << "Promise chain started!";
})
.delay(1000)
.then([ctx]() {
  // Like above but with an explicit timer
  auto timer = new QTimer(ctx);
  timer->start(1000);
  return makeConnectionPromise(timer, &QTimer::timeout);
})
.then(selectUrlPromise())
.then([ctx](const QUrl &url) {
  if (!url.isValid()) {
    throw PromiseError("No URL has been selected");
  }

  qDebug() << "Downloading" << url.toString() << "...";

  QNetworkRequest request(url);
  auto nam = new QNetworkAccessManager(ctx);
  ctx->reply = nam->get(request);

  Deferred<void> defer;
  defer.connect(ctx->reply, &QNetworkReply::downloadProgress, [ctx](qint64 bytesReceived, qint64 bytesTotal) {
    if (bytesReceived != ctx->bytesReceived) {
      double progress = double(bytesReceived) / double(bytesTotal);
      qDebug() << "Download progress:" << int(progress * 100.0) << "%";
      ctx->bytesReceived = bytesReceived;
    }
  });
  defer.connectAndResolve(ctx->reply, &QNetworkReply::finished);
  return defer.promise();
})
.then([ctx]() {
  ctx->reply->deleteLater();

  if (ctx->reply->error() != QNetworkReply::NoError) {  
    qDebug() << "Download failed :(";
    throw PromiseError(ctx->reply->errorString());
  }

  qDebug() << "Download successful :)";
  return ctx->reply->readAll();
})
.fail([](const PromiseError &error) {
  // Called if any of the previous promise in the chain has failed
  qWarning() << "Error:" << error.message();
})
.finally([](QObject *ctx) {
  // Called on completion (when successfull or after first fail)
  if (!ctx) {
    qWarning() << "Context has been destroyed during promise chain execution";
  }

  // Clean-up data
  delete ctx;
});

// Chain previous promise
downloadPromise
.then([](const QByteArray &data) {
  auto processData = [](const QByteArray &data) {
    // CPU-intensive operation performed in a separate thread
    int sum = 0;
    for (const QChar &c : data) {
      sum += c.toLatin1();
    }
      
    return sum;
  };

  // Return a QFuture<int>
  return QtConcurrent::run(processData, data);
})
.then([](int sum) {
  qDebug() << "Calculated sum:" << sum;
})
.fail([]() {
  qDebug() << "Could not calculate sum :(";
});
.finally([]() {
  qDebug() << "Done!";
});
```


Installation
========

TODO


API
========

Promise
---------------

```c++
template <typename T>
class Promise
```

**Promise<T>() (if T = void)**
**Promise<T>(T value) (if T != void)**

Create a fulfilled promise with a value.

**Promise<RetVal> then(Functor func)**

Chain the current promise and return a new promise. The function is sent to the
queue of the current event loop (or the one corresponding to the current promise
context) as soon as the current promise has been resolved. If the current promise
has failed or has been cancelled, func() will not be called.

The new promise depends on the return value of func:
- Promise<ValueType>: returns a Promise<ValueType> which matches the state and the
value of the other promise
- QFuture<ValueType>: returns a Promise<ValueType> which will be resolved as soon as
the future is completed (or cancelled when the future has been cancelled)
- Any other value of type ValueType: returns a fulfilled promise with the same value

When a PromiseError() exception is thrown with the execution of func, 'then()' returns
a failed promise with the corresponding error.

**Promise<OtherPromiseType> then(const Promise<OtherPromiseType> &otherPromise) const**

Chain a promise with another promise: returns a new promise which matches the state and the
value of the given promise.

**Promise<void> delay(int ms) const**
**Promise<void> delay(std::chrono) const**

TODO

**Promise<T> fail() const**
**Promise<T> fail(const PromiseError &error)const**

Executed if the promise has failed due to a previous error during the execution of the
promise chain.

Note: the 'fail()' call does not change the state and the value of the returned promise.
The promise still has an error and the subsequent 'then()' operations will be skipped. It is
possible, however, to recover from an error by returning a new promise (e.g.
'Promise<int>(12)' or to change the error by throwing another PromiseError() inside the
'fail()' lambda.

**Promise<T> finally() const**
**Promise<T> finally(const QObject *ctx) const**

Executed after the current promise has been completed (successfully or after an error).

Note: the 'finally()' call does not change the state and the value of a promise.

**T value() const (if T != void)**

Note: returns the value of a fulfilled promise. If the promise is still pending or has
an error, a default constructed value is returned ('T()').

**PromiseError error() const**

Note: returns the error of a failing promise. If the promise is still pending or does not
have an error, a default 'PromiseError()' is returned.

**bool isPending() const**

**bool isFinished() const**

**bool isFulfilled() const**

**bool hasError() const**

**bool isCanceled() const**

**Promise<T> makePromise(MakePromiseFunc func)**

Helper function to create a Promise by providing the resolve() and reject() functions.

'func' should be a function with the following signature:
```c++
func(ResolveFunc resolve, RejectFunc reject) -> void
```

The code inside the given function is immediately executed. The resulting promise
is pending until resolve() or reject() has been called.

Example:
```c++
makePromise<int>([](auto resolve, auto reject) {
  QTimer::singleShot(10, [=]() {
    int randomValue = qrand();
    if (randomValue % 2) {
      resolve(randomValue);
    } else {
      reject(PromiseError("bad luck"));
    }
  });
});
```

ConnectionPromise
---------------

```c++
template <typename T>
class ConnectionPromise : public Promise<T>
```

**ConnectionPromise(QObject *emitter, PointerToObjectMethod signal)**

TODO:
Create a 'Promise' which will be resolved as soon as the given signal has been emitted. The promise
value is the one of the first parameter of the emitted signal (or 'void' if the signal does not
have any parameter).

**makeConnectionPromise(QObject *emitter, PointerToObjectMethod signal)**

Helper function to create a ConnectionPromise which is automatically resolved as soon as the signal
of a given object has been emitted.

```c++
template <typename T>
ConnectionPromise<T> makeConnectionPromise(QObject *emitter, PointerToObjectMethod signal)
```

PromiseError
---------------

```c++
class PromiseError
```

**PromiseError(const QString &msg, int id = -1)**

**PromiseError(const QVariant &data, const QString &msg, int id = -1)**

**bool isContextDestroyed() const**

Deferred
---------------

```c++
template <typename T>
class Defer
```

**Deferred()**

**Promise<T> promise() const**

**void resolve() (if T = void)**
**void resolve(T) (if T != void)**

**void reject()**

**void connect(QObject *emitter, PointerToObjectMethod signal, Functor func)**
**void connectAndResolve(QObject *emitter, PointerToObjectMethod signal)**
**void connectAndResolve(QObject *emitter, PointerToObjectMethod signal, const T &value)**
**void connectAndReject(QObject *emitter, PointerToObjectMethod signal, const PromiseError &error = PromiseError())**

The connections used by the defer are automatically closed after resolving (or rejecting) the promise.

PromiseContext
---------------

```c++
class PromiseContext
```

**PromiseContext(QObject *context)**
**PromiseContext(QSharedPointer *context)**
**PromiseContext(QThread *thread)**
**PromiseContext(QThread *threadPool)**

Start a promise chain with a context which is applied to all chained promises created within the
scope of the PromiseContext instance. The promises which have been chained after the PromiseContext
instance has been deleted will not use that context anymore.

Note: PromiseContext is only intented to be used as a temporary instance and it is not recommended to
store it as a variable.

**void then(...ARGS)**

PromiseGroup
---------------

TODO

```c++
template <typename T>
class PromiseGroup : public Promise<PromiseGroupResults>
```

**PromiseGroup(QObject *context)**

**PromiseGroup(QSharedPointer *context)**

**PromiseGroup(QThread *thread)**

**PromiseGroup(QThread *threadPool)**

**TODO: add + function**

**TODO: add + promise**

PromiseGroupResults
---------------

TODO

**template <typename ResultType>
ResultType at(int index) const**

**int count() const**

Tests
========

See `test` directory.


Author
========

Benoit Walter

Note: Source code partially based on Ben Lau's AsyncFuture (https://github.com/benlau/asyncfuture).


License
========

Apache License Version 2.0 (http://www.apache.org/licenses/)

