#ifdef _WIN32
#   include <cstdio>
#   include <stdlib.h>
#   include <vector>
#   include <QCoreApplication>
#   include <QFile>
#   include <QString>
#   include <QSysInfo>
#   include <QtGlobal>
#endif

#include "migration/migration.hpp"
#include "main-window.hpp"
#include "options/options.hpp"
using namespace options;
#include "opentrack-library-path.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStyleFactory>
#include <QStringList>
#include <QLocale>
#include <QTranslator>
#include <QDebug>
#include <memory>
#include <cstring>

#if /* denormal control */ \
    /* GNU */   defined __x86_64__  || defined __SSE2__ || \
    /* MSVC */  defined _M_AMD64    || (defined _M_IX86_FP && _M_IX86_FP >= 2)
#   include <xmmintrin.h>
#   include <pmmintrin.h>
#   include <float.h>

#define OTR_HAS_DENORM_CONTROL
void set_fp_mask()
{
    unsigned old_mask = _mm_getcsr();
    (void) old_mask;

    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    _MM_SET_ROUNDING_MODE(_MM_ROUND_NEAREST);
    _MM_SET_EXCEPTION_MASK(_MM_MASK_MASK);

#if 0
    unsigned new_mask = _mm_getcsr();

    qDebug() << "old" << (void*) old_mask << "new" << (void*) new_mask;
#endif
}
#endif

void set_qt_style()
{
#if defined _WIN32
    if (QSysInfo::WindowsVersion == QSysInfo::WV_XP)
        return;
#endif

#if defined(_WIN32) || defined(__APPLE__)
    // our layouts on OSX make some control wrongly sized -sh 20160908
    {
        const QStringList preferred { "fusion", "windowsvista", "macintosh" };
        for (const auto& style_name : preferred)
        {
            QStyle* s = QStyleFactory::create(style_name);
            if (s)
            {
                QApplication::setStyle(s);
                break;
            }
        }
    }
#endif
}

#ifdef _WIN32

void qdebug_to_console(QtMsgType, const QMessageLogContext& ctx, const QString &msg)
{
    const unsigned short* const str_ = msg.utf16();
    auto str = reinterpret_cast<const wchar_t* const>(str_);
    static_assert(sizeof(*str_) == sizeof(*str), "");

    std::fflush(stderr);
    if (ctx.function)
        std::fprintf(stderr, "[%s]: %ls\n", ctx.function, str);
    else if (ctx.file)
        std::fprintf(stderr, "[%s:%d]: %ls\n", ctx.file, ctx.line, str);
    else
        std::fprintf(stderr, "%ls\n", str);
    std::fflush(stderr);
}

void attach_parent_console()
{
    if (AttachConsole(ATTACH_PARENT_PROCESS))
    {
        // XXX c++ iostreams aren't reopened

        _wfreopen(L"CON", L"w", stdout);
        _wfreopen(L"CON", L"w", stderr);
        _wfreopen(L"CON", L"r", stdin);
        qInstallMessageHandler(qdebug_to_console);
    }
}

void add_win32_path()
{
    // see https://software.intel.com/en-us/articles/limitation-to-the-length-of-the-system-path-variable
    static char env_path[4096] { '\0', };
    {
        QString lib_path = OPENTRACK_BASE_PATH;
        lib_path.replace("/", "\\");
        const QByteArray lib_path_ = QFile::encodeName(lib_path);

        QString mod_path = OPENTRACK_BASE_PATH + OPENTRACK_LIBRARY_PATH;
        mod_path.replace("/", "\\");
        const QByteArray mod_path_ = QFile::encodeName(mod_path);

        std::vector<const char*> contents
        {
            "PATH=",
            lib_path_.constData(),
            ";",
            mod_path_.constData(),
            ";",
            getenv("PATH"),
        };

        bool ok = true;

        for (const char* ptr : contents)
        {
            if (ptr)
                strcat_s(env_path, sizeof(env_path), ptr);

            if (!ptr || ptr[0] == '\0' || env_path[0] == '\0')
            {
                qDebug() << "bad path element"
                         << (ptr == nullptr ? "<null>" : ptr);
                ok = false;
                break;
            }
        }

        if (ok)
        {
            const int error = _putenv(env_path);

            if (error)
                qDebug() << "can't _putenv win32 path";
        }
        else
            qDebug() << "can't set win32 path";
    }
}

#endif

int
#ifdef _MSC_VER
WINAPI
#endif
main(int argc, char** argv)
{
#ifdef _WIN32
    attach_parent_console();
#endif

#if defined OTR_HAS_DENORM_CONTROL
    set_fp_mask();
#endif

#if QT_VERSION >= 0x050600 // flag introduced in QT 5.6. It is non-essential so might as well allow compilation on older systems.
    QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
#endif
    QApplication::setAttribute(Qt::AA_X11InitThreads, true);

    QApplication app(argc, argv);

#ifdef _WIN32
    add_win32_path();
#endif

    MainWindow::set_working_directory();

#if !defined(__linux) && !defined _WIN32
    // workaround QTBUG-38598
    QCoreApplication::addLibraryPath(".");
#endif

    set_qt_style();
    QTranslator t;

    // QLocale::setDefault(QLocale("ru_RU")); // force i18n for testing

    if (group::with_global_settings_object([&](QSettings& s) {
        return !s.value("disable-translation", false).toBool();
    }))
    {
        (void) t.load(QLocale(), "", "", OPENTRACK_BASE_PATH + "/" OPENTRACK_I18N_PATH, ".qm");
        (void) QCoreApplication::installTranslator(&t);
    }

    do
    {
       std::shared_ptr<MainWindow> w = std::make_shared<MainWindow>();

       if (!w->isEnabled())
           break;

       if (!w->start_in_tray())
       {
           w->setVisible(true);
           w->show();
           w->adjustSize();
           w->setFixedSize(w->size());
       }
       else
           w->setVisible(false);

       app.setQuitOnLastWindowClosed(false);
       app.exec();

       app.exit(0);

       qDebug() << "exit: window";
    }
    while (false);

    // msvc crashes in Qt plugin system's dtor
    // Note: QLibrary::PreventUnloadHint seems to workaround it
#if defined(_MSC_VER) && 0
    qDebug() << "exit: terminating";
    TerminateProcess(GetCurrentProcess(), 0);
#endif

    qDebug() << "exit: main()";

    return 0;
}

#if defined(Q_CREATOR_RUN)
#   pragma clang diagnostic ignored "-Wmain"
#endif

#ifdef _MSC_VER
int WINAPI
WinMain (struct HINSTANCE__*, struct HINSTANCE__*, char*, int)
{
  return main (__argc, __argv);
}

#endif
