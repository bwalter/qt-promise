/****************************************************************************
**
** Copyright (C) 2017 Benoit Walter
** Contact: benoit.walter@meerun.com
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

#include "promisetests.h"
#include "../promise.h"
#include <QCoreApplication>
#include <QScopedPointer>
#include <QSemaphore>
#include <QTest>
#include <QTimer>
#include <QtConcurrent>
#include <algorithm>

using namespace QtPromise;

QTEST_MAIN(PromiseTests)

#define COMPARE_NO_RETURN(actual, expected) \
do {\
  QTest::qCompare(actual, expected, #actual, #expected, __FILE__, __LINE__);\
} while (0)

#define FAIL_NO_RETURN(msg) \
do {\
  QTest::qFail(msg, __FILE__, __LINE__);\
} while (0)

PromiseTests::PromiseTests(QObject *parent)
 : QObject(parent) {
}

void PromiseTests::cleanup()
{
  // Process events (for deferred deletes using QObject::deleteLater())
  do {
    QCoreApplication::sendPostedEvents();
    QCoreApplication::sendPostedEvents( 0, QEvent::DeferredDelete );
    QCoreApplication::processEvents();
  }
  while ( QCoreApplication::hasPendingEvents() );
}

void PromiseTests::test_promiseConstructor() {
  Promise<void> voidPromise;
  QVERIFY(!voidPromise.isPending());
  QVERIFY(voidPromise.isFinished());
  QVERIFY(voidPromise.isFulfilled());
  QVERIFY(!voidPromise.hasError());

  Promise<int> intPromise(32);
  QVERIFY(!intPromise.isPending());
  QVERIFY(intPromise.isFinished());
  QVERIFY(intPromise.isFulfilled());
  QVERIFY(!intPromise.hasError());
  QCOMPARE(intPromise.value(), 32);

  Promise<QString> stringPromise("defaultValue");
  QVERIFY(!stringPromise.isPending());
  QVERIFY(stringPromise.isFinished());
  QVERIFY(stringPromise.isFulfilled());
  QVERIFY(!stringPromise.hasError());
  QCOMPARE(stringPromise.value(), QString("defaultValue"));
}

void PromiseTests::test_successfullPromise() {
  bool done = false;
  int value = 0;
  QThread *mainThread = QThread::currentThread();

  auto promise = Promise<void>()
  .then([&]() {
    COMPARE_NO_RETURN(QThread::currentThread(), mainThread);
    ++value;
    return value;
  })
  .then([&](int v) {
    COMPARE_NO_RETURN(QThread::currentThread(), mainThread);
    value = v + 1;
    return value;
  })
  .fail([&]() {
    QFAIL("Fail lambda should have been skipped");
    value = -1;
  })
  .fail([&](const PromiseError &) {
    QFAIL("Fail lambda should have been skipped");
    value = -1;
  })
  .finally([&]() {
    QCOMPARE(QThread::currentThread(), mainThread);
    value = 9;
  })
  .finally([&](const QObject *contextObject) {
    QCOMPARE(QThread::currentThread(), mainThread);
    QCOMPARE(contextObject, nullptr);
    done = true;
    value = 10;
  });

  QCOMPARE(promise.contextObject(), nullptr);

  // Before running
  QVERIFY(promise.isPending());
  QVERIFY(!promise.isFinished());
  QVERIFY(!promise.isFulfilled());
  QVERIFY(!promise.isCanceled());
  QVERIFY(!promise.hasError());
  QCOMPARE(value, 0);
  QCOMPARE(promise.value(), int());

  // First then
  qApp->processEvents();
  QCOMPARE(value, 1);

  // Second then
  qApp->processEvents();
  QCOMPARE(value, 2);

  // 2x Fail (skipped)
  qApp->processEvents();
  qApp->processEvents();
  QCOMPARE(value, 2);

  // 2x Finally
  qApp->processEvents();
  QCOMPARE(value, 9);
  qApp->processEvents();
  QCOMPARE(value, 10);

  // After running
  QVERIFY(!promise.isPending());
  QVERIFY(promise.isFinished());
  QVERIFY(promise.isFulfilled());
  QVERIFY(!promise.isCanceled());
  QVERIFY(!promise.hasError());
  QVERIFY(promise.error().message().isEmpty());
  QCOMPARE(promise.value(), 2);
  QVERIFY(done);

  // ---

  // Chain resulting promise and return a string
  auto otherPromise = promise
  .then([&](int v) {
    COMPARE_NO_RETURN(v, 2);
    value = 3;
    return QString("stringValue");
  })
  .fail([&]() {
    QFAIL("Fail lambda should have been skipped");
    value = -2;
  });

  // Before running
  QCOMPARE(promise.value(), 2);
  QCOMPARE(otherPromise.value(), QString());

  // First then
  qApp->processEvents();
  QCOMPARE(value, 3);

  // Fail (skipped)
  qApp->processEvents();
  QCOMPARE(value, 3);

  // After running
  QCOMPARE(promise.value(), 2);  // initial promise value has not changed
  QCOMPARE(otherPromise.value(), QString("stringValue"));
}

void PromiseTests::test_failingPromise() {
  bool done = false;
  int value = 0;
  const QString errorMessage = "Test error message";
  QThread *mainThread = QThread::currentThread();

  auto promise = Promise<void>()
  .then([&]() {
    QCOMPARE(QThread::currentThread(), mainThread);
    ++value;
    throw PromiseError(errorMessage);
  })
  .then([&]() {
    FAIL_NO_RETURN("Then lambda should have been skipped");
    ++value;
    return value;
  })
  .fail([&]() {
    QCOMPARE(QThread::currentThread(), mainThread);
    value = -1;
  })
  .fail([&](const PromiseError &promiseError) {
    QCOMPARE(QThread::currentThread(), mainThread);
    QCOMPARE(promiseError.message(), errorMessage);
    value = -2;
  })
  .finally([&]() {
    QCOMPARE(QThread::currentThread(), mainThread);
    done = true;
    value = 9;
  })
  .finally([&](const QObject *contextObject) {
    QCOMPARE(QThread::currentThread(), mainThread);
    QCOMPARE(contextObject, nullptr);
    done = true;
    value = 10;
  });

  // Before running
  QVERIFY(promise.isPending());
  QVERIFY(!promise.isFinished());
  QVERIFY(!promise.isFulfilled());
  QVERIFY(!promise.isCanceled());
  QVERIFY(!promise.hasError());
  QCOMPARE(value, 0);
  QCOMPARE(promise.value(), int());

  // First then
  qApp->processEvents();
  QCOMPARE(value, 1);

  // Second then (skipped)
  qApp->processEvents();
  QCOMPARE(value, 1);

  // 2x Fail
  qApp->processEvents();
  QCOMPARE(value, -1);
  qApp->processEvents();
  QCOMPARE(value, -2);

  // 2x Finally
  qApp->processEvents();
  QCOMPARE(value, 9);
  qApp->processEvents();
  QCOMPARE(value, 10);

  // After running
  QVERIFY(!promise.isPending());
  QVERIFY(promise.isFinished());
  QVERIFY(!promise.isFulfilled());
  QVERIFY(!promise.isCanceled());
  QVERIFY(promise.hasError());
  QCOMPARE(promise.error().message(), errorMessage);
  QCOMPARE(promise.value(), int());
  QVERIFY(done);

  // ---

  // Chain resulting promise and return a string
  auto otherPromise = promise
  .then([&](int) {
    FAIL_NO_RETURN("Then lambda should have been skipped");
    value = 3;
    return QString("stringValue");
  })
  .fail([&]() {
    value = -2;
  });

  // Before running
  QVERIFY(!otherPromise.hasError());

  // First then (skipped)
  qApp->processEvents();
  QCOMPARE(value, 10);

  // Fail
  qApp->processEvents();
  QCOMPARE(value, -2);

  // After running
  QVERIFY(otherPromise.hasError());
  QCOMPARE(otherPromise.error().message(), errorMessage);
}

void PromiseTests::test_makePromiseAndResolve() {
  auto promise = makePromise<int>([](auto resolve, auto reject) {
    Q_UNUSED(reject);

    QTimer::singleShot(0, [=]() {
      resolve(10);
    });
  });

  QVERIFY(promise.isPending());
  QVERIFY(!promise.isFulfilled());

  qApp->processEvents();

  // should have been fulfilled
  QVERIFY(promise.isFulfilled());
  QVERIFY(!promise.hasError());
  QVERIFY(!promise.isCanceled());
  QCOMPARE(promise.value(), 10);
}

void PromiseTests::test_makePromiseAndReject() {
  auto promise = makePromise<int>([](auto resolve, auto reject) {
    Q_UNUSED(resolve);

    QTimer::singleShot(0, [=]() {
      reject(PromiseError("TestError"));
    });
  });

  QVERIFY(promise.isPending());

  qApp->processEvents();

  // should have an error
  QVERIFY(!promise.isPending());
  QVERIFY(!promise.isFulfilled());
  QVERIFY(promise.hasError());
  QCOMPARE(promise.error().message(), QString("TestError"));
}

void PromiseTests::test_multipleChain() {
  int value1 = 0;
  int value2 = 0;
  int value3 = 0;

  Deferred<int> defer;
  auto promise = defer.promise();

  auto promise1 = promise.then([&](int v) { value1 = v; });
  auto promise2 = promise.then([&](int v) { value2 = v; });
  defer.resolve(10);
  auto promise3 = promise.then([&](int v) { value3 = v; });

  qApp->processEvents();
  QCOMPARE(value1, 10);
  QCOMPARE(value2, 10);
  QCOMPARE(value3, 10);
}

void PromiseTests::test_resolvedDefer() {
  Deferred<int> defer;
  auto promise = defer.promise();

  QVERIFY(promise.isPending());
  QVERIFY(!promise.isFinished());

  defer.resolve(1664);

  QVERIFY(!promise.isPending());
  QVERIFY(promise.isFinished());
  QVERIFY(promise.isFulfilled());
  QVERIFY(!promise.hasError());
  QCOMPARE(promise.value(), 1664);
}

void PromiseTests::test_rejectedDefer() {
  Deferred<int> defer;
  auto promise = defer.promise();

  QVERIFY(promise.isPending());
  QVERIFY(!promise.isFinished());

  defer.reject(PromiseError("Deferred error"));

  QVERIFY(!promise.isPending());
  QVERIFY(promise.isFinished());
  QVERIFY(!promise.isFulfilled());
  QVERIFY(promise.hasError());
  QCOMPARE(promise.error().message(), QString("Deferred error"));
}

void PromiseTests::test_contextThread() {
  QThread *mainThread = QThread::currentThread();
  QThread *thread = new QThread();
  thread->start();

  QSemaphore semaphore(0);
  int value = 0;

  auto promise = PromiseContext(thread)
  .then([&]() {
    QCOMPARE(QThread::currentThread(), thread);
    semaphore.acquire(1);
    value = 1;
  })
  .then([&]() {
    QCOMPARE(QThread::currentThread(), thread);
    semaphore.acquire(1);
    value = 2;
  })
  .finally([&](QObject *contextObject) {
    QCOMPARE(QThread::currentThread(), thread);
    QCOMPARE(contextObject, thread);
    semaphore.acquire(1);
    value = 3;
  });

  QCOMPARE(value, 0);

  qApp->processEvents();
  semaphore.release(1);
  QTRY_COMPARE(value, 1);

  qApp->processEvents();
  semaphore.release(1);
  QTRY_COMPARE(value, 2);

  qApp->processEvents();
  semaphore.release(1);
  QTRY_COMPARE(value, 3);
  
  // After promise chain scope, no context anymore
  // => back to main thread!
  promise.then([&]() {
    QCOMPARE(QThread::currentThread(), mainThread);
    value = 4;
  });

  QTRY_COMPARE(value, 4);

  thread->exit();
  thread->wait();
  thread->deleteLater();
}

void PromiseTests::test_contextObject() {
  // e.g. context destroyed

  QThread *mainThread = QThread::currentThread();

  class ContextObject : public QObject {
    public:
      int value = 0;
  };
  auto ctx = new ContextObject();

  // Promise chain with the above context object
  auto promise = PromiseContext(ctx)
  .then([=]() {
    QCOMPARE(QThread::currentThread(), mainThread);
    ctx->value = 1;
  })
  .finally([&](QObject *contextObject) {
    QCOMPARE(QThread::currentThread(), mainThread);
    QCOMPARE(contextObject, ctx);
    QCOMPARE(ctx->value, 1);
    ctx->value = 2;
  });

  QCOMPARE(ctx->value, 0);

  // First then
  qApp->processEvents();
  QCOMPARE(ctx->value, 1);

  // Finally
  qApp->processEvents();
  QCOMPARE(ctx->value, 2);

  // After promise chain scope, no context anymore
  promise.finally([&](QObject *contextObject) {
    QCOMPARE(contextObject, nullptr);
    ctx->value = 3;
  });

  // Finally
  qApp->processEvents();
  QCOMPARE(ctx->value, 3);

  bool failReached = false;
  bool finallyReached = false;

  // Another promise chain with the context object
  promise = PromiseContext(ctx)
  .then([=]() {
    ctx->value = 4;
  })
  .then([]() {
    QFAIL("Should not reach then after context destruction");
  })
  .fail([&](const PromiseError &promiseError) {
    failReached = true;
    QVERIFY(promiseError.isContextDestroyed());
  })
  .finally([&](QObject *contextObject) {
    finallyReached = true;
    QCOMPARE(contextObject, nullptr);
  });

  QCOMPARE(ctx->value, 3);

  // First then
  qApp->processEvents();
  QCOMPARE(ctx->value, 4);

  // Destroy context
  delete ctx;
  ctx = nullptr;

  // Fail
  qApp->processEvents();
  QVERIFY(failReached);
  QVERIFY(!finallyReached);

  // Finally
  qApp->processEvents();
  QVERIFY(finallyReached);
}

void PromiseTests::test_future() {
  QMutex mutex;
  bool insideFuture = false;

  auto promise = PromiseContext(nullptr)
  .then([&]() {
    auto func = [&]() {
      insideFuture = true;
      mutex.lock();
      mutex.unlock();
      return 12;
    };

    return QtConcurrent::run(func);
  })
  .then([](int value) {
    return value;
  });

  QVERIFY(promise.isPending());
  QVERIFY(!insideFuture);

  mutex.lock();
  QTRY_VERIFY(insideFuture);

  QVERIFY(promise.isPending());
  mutex.unlock();

  QTRY_VERIFY(promise.isFulfilled());
  QCOMPARE(promise.value(), 12);
}

void PromiseTests::test_canceledFuture() {
  auto promise = PromiseContext(nullptr)
  .then([]() {
    return QFuture<bool>();  // canceled future
  });

  QVERIFY(promise.isPending());

  qApp->processEvents();
  QVERIFY(!promise.isPending());
  QVERIFY(!promise.isFulfilled());
  QVERIFY(promise.isCanceled());
}

void PromiseTests::test_embeddedPromise() {
  Deferred<bool> defer1;
  auto promise1 = defer1.promise();

  auto promise2 = PromiseContext(nullptr)
  .then([=]() {
    return promise1;
  });

  QVERIFY(promise1.isPending());
  QVERIFY(promise2.isPending());

  // In then
  qApp->processEvents();
  QVERIFY(promise1.isPending());
  QVERIFY(promise2.isPending());

  // Resolve first promise
  defer1.resolve(true);
  QVERIFY(promise1.isFulfilled());
  QVERIFY(promise2.isPending());

  qApp->processEvents();

  QVERIFY(promise2.isFulfilled());
  QCOMPARE(promise2.value(), true);
}

void PromiseTests::test_connection() {
  int intValue = 0;
  QString strValue;

  PromiseContext(nullptr)
  .then([&]() {
    intValue = 1;
    return makeConnectionPromise(this, &PromiseTests::testVoidSignal);
  })
  .then([&]() {
    intValue = 2;
    return makeConnectionPromise(this, &PromiseTests::testIntSignal);
  })
  .then([&](int i) {
    intValue = i;
    return makeConnectionPromise(this, &PromiseTests::testStringSignal);
  })
  .then([&](const QString &s) {
    strValue = s;
    return makeConnectionPromise(this, &PromiseTests::testIntStringSignal);
  })
  .then([&](int i) {
    intValue = i;
  });

  QCOMPARE(intValue, 0);
  QVERIFY(strValue.isEmpty());

  // Then
  qApp->processEvents();
  QCOMPARE(intValue, 1);

  // Emit void signal
  emit testVoidSignal();

  // Then
  QTRY_COMPARE(intValue, 2);

  // Emit int signal
  emit testIntSignal(10);

  // Then
  QTRY_COMPARE(intValue, 10);

  // Emit string signal
  emit testStringSignal("testString");

  // Then
  QTRY_COMPARE(strValue, QString("testString"));

  // Emit int+string signal
  emit testIntStringSignal(20, "unusedString");

  // Then
  QTRY_COMPARE(intValue, 20);
}

void PromiseTests::test_deferredConnect() {
  int value = 0;

  Deferred<void> defer;
  defer.connect(this, &PromiseTests::testIntSignal, [&value](int v) {
    QVERIFY(v <= 3);
    value = v;
  });
  auto promise = defer.promise();

  QVERIFY(promise.isPending());

  emit testIntSignal(1);
  QCOMPARE(value, 1);

  emit testIntSignal(2);
  QCOMPARE(value, 2);

  emit testIntSignal(3);
  QCOMPARE(value, 3);

  defer.resolve();

  // Not called anymore after resolve
  emit testIntSignal(4);
  qApp->processEvents();
  QCOMPARE(value, 3);

  emit testIntSignal(5);
  qApp->processEvents();
  QCOMPARE(value, 3);
}

void PromiseTests::test_deferredConnectAndResolve() {
  // void deferred with argument-less signal
  {
    Deferred<void> defer;
    defer.connectAndResolve(this, &PromiseTests::testVoidSignal);
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testVoidSignal();
    QTRY_VERIFY(promise.isFulfilled());
  }

  // void deferred with (int) signal
  {
    Deferred<void> defer;
    defer.connectAndResolve(this, &PromiseTests::testIntSignal);
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testIntSignal(10);
    QTRY_VERIFY(promise.isFulfilled());
  }

  // int deferred with (int) signal and implicit value
  {
    Deferred<int> defer;
    defer.connectAndResolve(this, &PromiseTests::testIntSignal);
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testIntSignal(10);
    QTRY_VERIFY(promise.isFulfilled());
    QCOMPARE(promise.value(), 10);
  }

  // int deferred with (void) signal and explicit value
  {
    Deferred<int> defer;
    defer.connectAndResolve(this, &PromiseTests::testVoidSignal, 20);
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testVoidSignal();
    QTRY_VERIFY(promise.isFulfilled());
    QCOMPARE(promise.value(), 20);
  }

  // int deferred with (int) signal and explicit value
  {
    Deferred<int> defer;
    defer.connectAndResolve(this, &PromiseTests::testIntSignal, 20);
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testIntSignal(10);
    QTRY_VERIFY(promise.isFulfilled());
    QCOMPARE(promise.value(), 20);
  }
}

void PromiseTests::test_deferredConnectAndReject() {
  // Without explicit error
  {
    Deferred<void> defer;
    defer.connectAndReject(this, &PromiseTests::testIntSignal);
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testIntSignal(10);
    QTRY_VERIFY(promise.hasError());
    QVERIFY(promise.error().message().isEmpty());
  }

  // With explicit error
  {
    Deferred<void> defer;
    defer.connectAndReject(this, &PromiseTests::testIntSignal, PromiseError("testError"));
    auto promise = defer.promise();

    QVERIFY(promise.isPending());

    emit testIntSignal(10);
    QTRY_VERIFY(promise.hasError());
    QCOMPARE(promise.error().message(), QString("testError"));
  }
}
