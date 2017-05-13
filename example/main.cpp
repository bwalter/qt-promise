#include <QCoreApplication>
#include <QMetaObject>
#include "promiseexample.h"

int main(int argc, char *argv[])
{
    QCoreApplication a(argc, argv);

    PromiseExample *example = new PromiseExample();
    QMetaObject::invokeMethod(example, "init");

    return a.exec();
}
