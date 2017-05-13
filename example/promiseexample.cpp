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

#include "promiseexample.h"
#include "promise.h"
#include <QtConcurrent>
#include <QCoreApplication>
#include <QImage>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QSharedPointer>
#include <QTimer>

using namespace QtPromise;

static Promise<QUrl> selectUrlPromise();

PromiseExample::PromiseExample(QObject *parent)
 : QObject(parent)
{
}

PromiseExample::~PromiseExample()
{
}

void PromiseExample::init()
{
  // Context object with data used inside the promise chain and whose lifecycle
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
  //.delay(1000)
  .then([ctx]() {
    // Like above but with an explicit timer
    auto timer = new QTimer(ctx);
    timer->start(1000);
    return makeConnectionPromise(timer, &QTimer::timeout);
  })
  //.then(selectUrlPromise())
  .then([]() {
    return selectUrlPromise();
  })
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
      qDebug() << "Download failed.";
      throw PromiseError(ctx->reply->errorString());
    }

    qDebug() << "Download successful!";
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
  })
  .finally([]() {
    qApp->exit();
  });
}

Promise<QUrl> selectUrlPromise() {
  return Promise<QUrl>(QUrl("http://www.meerun.com"));
}
