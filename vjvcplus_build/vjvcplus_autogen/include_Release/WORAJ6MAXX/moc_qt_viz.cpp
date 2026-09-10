/****************************************************************************
** Meta object code from reading C++ file 'qt_viz.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.8.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../../VJVCPlusCpp/src/viz/qt_viz.h"
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'qt_viz.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.8.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN2vj9QtVizSinkE_t {};
} // unnamed namespace


#ifdef QT_MOC_HAS_STRINGDATA
static constexpr auto qt_meta_stringdata_ZN2vj9QtVizSinkE = QtMocHelpers::stringData(
    "vj::QtVizSink",
    "trackChanged",
    "",
    "binsChanged",
    "statusChanged",
    "standbyPathChanged",
    "bgVideoPathChanged",
    "closeRequested",
    "standbyColorsChanged",
    "effectiveColorsChanged",
    "sigTrack",
    "valid",
    "title",
    "artist",
    "album",
    "coverPath",
    "tentative",
    "confidence",
    "QVariantList",
    "colors",
    "sigSpectrum",
    "bins",
    "peak",
    "sigStatus",
    "statusText",
    "requestClose",
    "setStandbyPath",
    "p",
    "setBgVideoPath",
    "hasTrack",
    "standbyPath",
    "bgVideoPath",
    "trackColor",
    "trackColors",
    "standbyColors",
    "effectiveColors"
);
#else  // !QT_MOC_HAS_STRINGDATA
#error "qtmochelpers.h not found or too old."
#endif // !QT_MOC_HAS_STRINGDATA

Q_CONSTINIT static const uint qt_meta_data_ZN2vj9QtVizSinkE[] = {

 // content:
      12,       // revision
       0,       // classname
       0,    0, // classinfo
      14,   14, // methods
      16,  138, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      11,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    0,   98,    2, 0x06,   17 /* Public */,
       3,    0,   99,    2, 0x06,   18 /* Public */,
       4,    0,  100,    2, 0x06,   19 /* Public */,
       5,    0,  101,    2, 0x06,   20 /* Public */,
       6,    0,  102,    2, 0x06,   21 /* Public */,
       7,    0,  103,    2, 0x06,   22 /* Public */,
       8,    0,  104,    2, 0x06,   23 /* Public */,
       9,    0,  105,    2, 0x06,   24 /* Public */,
      10,    8,  106,    2, 0x06,   25 /* Public */,
      20,    2,  123,    2, 0x06,   34 /* Public */,
      23,    1,  128,    2, 0x06,   37 /* Public */,

 // methods: name, argc, parameters, tag, flags, initial metatype offsets
      25,    0,  131,    2, 0x02,   39 /* Public */,
      26,    1,  132,    2, 0x02,   40 /* Public */,
      28,    1,  135,    2, 0x02,   42 /* Public */,

 // signals: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::Bool, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::Bool, QMetaType::Float, 0x80000000 | 18,   11,   12,   13,   14,   15,   16,   17,   19,
    QMetaType::Void, 0x80000000 | 18, QMetaType::Float,   21,   22,
    QMetaType::Void, QMetaType::QString,   24,

 // methods: parameters
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,   27,
    QMetaType::Void, QMetaType::QString,   27,

 // properties: name, type, flags, notifyId, revision
      29, QMetaType::Bool, 0x00015001, uint(0), 0,
      12, QMetaType::QString, 0x00015001, uint(0), 0,
      13, QMetaType::QString, 0x00015001, uint(0), 0,
      14, QMetaType::QString, 0x00015001, uint(0), 0,
      16, QMetaType::Bool, 0x00015001, uint(0), 0,
      17, QMetaType::Float, 0x00015001, uint(0), 0,
      15, QMetaType::QString, 0x00015001, uint(0), 0,
      21, 0x80000000 | 18, 0x00015009, uint(1), 0,
      22, QMetaType::Float, 0x00015001, uint(1), 0,
      24, QMetaType::QString, 0x00015001, uint(2), 0,
      30, QMetaType::QString, 0x00015001, uint(3), 0,
      31, QMetaType::QString, 0x00015001, uint(4), 0,
      32, QMetaType::QColor, 0x00015001, uint(0), 0,
      33, 0x80000000 | 18, 0x00015009, uint(0), 0,
      34, 0x80000000 | 18, 0x00015009, uint(6), 0,
      35, 0x80000000 | 18, 0x00015009, uint(7), 0,

       0        // eod
};

Q_CONSTINIT const QMetaObject vj::QtVizSink::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_ZN2vj9QtVizSinkE.offsetsAndSizes,
    qt_meta_data_ZN2vj9QtVizSinkE,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_tag_ZN2vj9QtVizSinkE_t,
        // property 'hasTrack'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'title'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'artist'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'album'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'tentative'
        QtPrivate::TypeAndForceComplete<bool, std::true_type>,
        // property 'confidence'
        QtPrivate::TypeAndForceComplete<float, std::true_type>,
        // property 'coverPath'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'bins'
        QtPrivate::TypeAndForceComplete<QVariantList, std::true_type>,
        // property 'peak'
        QtPrivate::TypeAndForceComplete<float, std::true_type>,
        // property 'statusText'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'standbyPath'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'bgVideoPath'
        QtPrivate::TypeAndForceComplete<QString, std::true_type>,
        // property 'trackColor'
        QtPrivate::TypeAndForceComplete<QColor, std::true_type>,
        // property 'trackColors'
        QtPrivate::TypeAndForceComplete<QVariantList, std::true_type>,
        // property 'standbyColors'
        QtPrivate::TypeAndForceComplete<QVariantList, std::true_type>,
        // property 'effectiveColors'
        QtPrivate::TypeAndForceComplete<QVariantList, std::true_type>,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<QtVizSink, std::true_type>,
        // method 'trackChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'binsChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'statusChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'standbyPathChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'bgVideoPathChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'closeRequested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'standbyColorsChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'effectiveColorsChanged'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'sigTrack'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        QtPrivate::TypeAndForceComplete<float, std::false_type>,
        QtPrivate::TypeAndForceComplete<QVariantList, std::false_type>,
        // method 'sigSpectrum'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QVariantList, std::false_type>,
        QtPrivate::TypeAndForceComplete<float, std::false_type>,
        // method 'sigStatus'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QString, std::false_type>,
        // method 'requestClose'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'setStandbyPath'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>,
        // method 'setBgVideoPath'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QString &, std::false_type>
    >,
    nullptr
} };

void vj::QtVizSink::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<QtVizSink *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->trackChanged(); break;
        case 1: _t->binsChanged(); break;
        case 2: _t->statusChanged(); break;
        case 3: _t->standbyPathChanged(); break;
        case 4: _t->bgVideoPathChanged(); break;
        case 5: _t->closeRequested(); break;
        case 6: _t->standbyColorsChanged(); break;
        case 7: _t->effectiveColorsChanged(); break;
        case 8: _t->sigTrack((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[3])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[4])),(*reinterpret_cast< std::add_pointer_t<QString>>(_a[5])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[6])),(*reinterpret_cast< std::add_pointer_t<float>>(_a[7])),(*reinterpret_cast< std::add_pointer_t<QVariantList>>(_a[8]))); break;
        case 9: _t->sigSpectrum((*reinterpret_cast< std::add_pointer_t<QVariantList>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<float>>(_a[2]))); break;
        case 10: _t->sigStatus((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 11: _t->requestClose(); break;
        case 12: _t->setStandbyPath((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        case 13: _t->setBgVideoPath((*reinterpret_cast< std::add_pointer_t<QString>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::trackChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::binsChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::statusChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::standbyPathChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::bgVideoPathChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::closeRequested; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::standbyColorsChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)();
            if (_q_method_type _q_method = &QtVizSink::effectiveColorsChanged; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)(bool , QString , QString , QString , QString , bool , float , QVariantList );
            if (_q_method_type _q_method = &QtVizSink::sigTrack; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 8;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)(QVariantList , float );
            if (_q_method_type _q_method = &QtVizSink::sigSpectrum; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 9;
                return;
            }
        }
        {
            using _q_method_type = void (QtVizSink::*)(QString );
            if (_q_method_type _q_method = &QtVizSink::sigStatus; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 10;
                return;
            }
        }
    }
    if (_c == QMetaObject::ReadProperty) {
        void *_v = _a[0];
        switch (_id) {
        case 0: *reinterpret_cast< bool*>(_v) = _t->hasTrack(); break;
        case 1: *reinterpret_cast< QString*>(_v) = _t->title(); break;
        case 2: *reinterpret_cast< QString*>(_v) = _t->artist(); break;
        case 3: *reinterpret_cast< QString*>(_v) = _t->album(); break;
        case 4: *reinterpret_cast< bool*>(_v) = _t->tentative(); break;
        case 5: *reinterpret_cast< float*>(_v) = _t->confidence(); break;
        case 6: *reinterpret_cast< QString*>(_v) = _t->coverPath(); break;
        case 7: *reinterpret_cast< QVariantList*>(_v) = _t->bins(); break;
        case 8: *reinterpret_cast< float*>(_v) = _t->peak(); break;
        case 9: *reinterpret_cast< QString*>(_v) = _t->statusText(); break;
        case 10: *reinterpret_cast< QString*>(_v) = _t->standbyPath(); break;
        case 11: *reinterpret_cast< QString*>(_v) = _t->bgVideoPath(); break;
        case 12: *reinterpret_cast< QColor*>(_v) = _t->trackColor(); break;
        case 13: *reinterpret_cast< QVariantList*>(_v) = _t->trackColors(); break;
        case 14: *reinterpret_cast< QVariantList*>(_v) = _t->standbyColors(); break;
        case 15: *reinterpret_cast< QVariantList*>(_v) = _t->effectiveColors(); break;
        default: break;
        }
    }
}

const QMetaObject *vj::QtVizSink::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *vj::QtVizSink::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ZN2vj9QtVizSinkE.stringdata0))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "VizSink"))
        return static_cast< VizSink*>(this);
    return QObject::qt_metacast(_clname);
}

int vj::QtVizSink::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 14)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 14;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 14)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 14;
    }
    if (_c == QMetaObject::ReadProperty || _c == QMetaObject::WriteProperty
            || _c == QMetaObject::ResetProperty || _c == QMetaObject::BindableProperty
            || _c == QMetaObject::RegisterPropertyMetaType) {
        qt_static_metacall(this, _c, _id, _a);
        _id -= 16;
    }
    return _id;
}

// SIGNAL 0
void vj::QtVizSink::trackChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 0, nullptr);
}

// SIGNAL 1
void vj::QtVizSink::binsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void vj::QtVizSink::statusChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 2, nullptr);
}

// SIGNAL 3
void vj::QtVizSink::standbyPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 3, nullptr);
}

// SIGNAL 4
void vj::QtVizSink::bgVideoPathChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 4, nullptr);
}

// SIGNAL 5
void vj::QtVizSink::closeRequested()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void vj::QtVizSink::standbyColorsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void vj::QtVizSink::effectiveColorsChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 7, nullptr);
}

// SIGNAL 8
void vj::QtVizSink::sigTrack(bool _t1, QString _t2, QString _t3, QString _t4, QString _t5, bool _t6, float _t7, QVariantList _t8)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t6))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t7))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t8))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void vj::QtVizSink::sigSpectrum(QVariantList _t1, float _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void vj::QtVizSink::sigStatus(QString _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 10, _a);
}
QT_WARNING_POP
