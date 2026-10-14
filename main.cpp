#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDirIterator>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLabel>
#include <QMessageBox>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QOffscreenSurface>
#include <QProgressBar>
#include <QPushButton>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QStorageInfo>
#include <QSurfaceFormat>
#include <QSet>
#include <QTemporaryDir>
#include <QUrl>
#include <QDesktopServices>
#include <QDateTime>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

class Launcher : public QWidget
{
public:
    Launcher()
    {
        setWindowTitle("Atrim Linux");
        resize(680, 520);
        setMinimumSize(560, 420);

        auto *layout = new QVBoxLayout(this);

        auto *intro = new QLabel(
            "<b>Atrim Linux</b><br>"
            "Install Atrim into an existing Amnesia: The Dark Descent folder."
        );
        intro->setWordWrap(true);

        folderLabel = new QLabel("No game folder selected");
        folderLabel->setWordWrap(true);
        folderLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        auto *repoLink = new QLabel(
            "<a href=\"https://github.com/roozerxc/atrim\">"
            "Atrim GitHub repository</a>"
        );
        repoLink->setOpenExternalLinks(true);

        scanButton = new QPushButton("Scan Steam and Heroic locations");
        scanButton->setToolTip("Search Steam libraries, Heroic and mount points for Amnesia installs");

        browseButton = new QPushButton("Choose game folder…");

        versionBox = new QComboBox;
        versionBox->addItem(
            "Stable — v1.4.5-win32",
            "https://github.com/roozerxc/atrim/releases/download/"
            "v1.4.5-win32/v1.4.5-win32-release.zip"
        );
        versionBox->addItem(
            "Beta — v1.4.6-win32",
            "https://github.com/roozerxc/atrim/releases/download/"
            "v1.4.6-win32-beta/v1.4.6-win32-beta.zip"
        );
        versionBox->addItem(
            "Beta — v1.4.6-win64",
            "https://github.com/roozerxc/atrim/releases/download/"
            "v1.4.6-win32-beta/v1.4.6-win64-beta.zip"
        );

        installButton = new QPushButton("Install selected Atrim version");
        installButton->setEnabled(false);
        installButton->setToolTip("Download and install the selected version into the game folder");

        auto *glButton = new QPushButton("Check OpenGL");
        glButton->setToolTip("Amnesia needs OpenGL 2.1 or newer");

        progressBar = new QProgressBar;
        progressBar->setRange(0, 0);
        progressBar->setVisible(false);

        statusLabel = new QLabel;
        statusLabel->setWordWrap(true);
        statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);

        layout->addWidget(intro);
        layout->addWidget(repoLink);
        layout->addWidget(folderLabel);
        layout->addWidget(scanButton);
        layout->addWidget(browseButton);
        layout->addWidget(new QLabel("Atrim version:"));
        layout->addWidget(versionBox);
        layout->addWidget(installButton);
        layout->addWidget(glButton);
        layout->addWidget(progressBar);
        layout->addWidget(statusLabel);
        layout->addStretch(1);

        connect(scanButton, &QPushButton::clicked,
                this, [this] { scanLocations(); });

        connect(browseButton, &QPushButton::clicked,
                this, [this] { chooseFolder(); });

        connect(installButton, &QPushButton::clicked,
                this, [this] { installSelected(); });

        connect(glButton, &QPushButton::clicked,
                this, [this] { checkOpenGL(); });

        // Restore last folder if it is still valid.
        QSettings settings("Atrim", "AtrimLinux");
        const QString last = settings.value("lastFolder").toString();
        if (!last.isEmpty() && isAmnesiaInstall(last))
            selectFolder(last);
    }

private:
    QLabel *folderLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QComboBox *versionBox = nullptr;
    QPushButton *installButton = nullptr;
    QPushButton *scanButton = nullptr;
    QPushButton *browseButton = nullptr;
    QProgressBar *progressBar = nullptr;
    QString selectedFolder;

    static bool fileExistsInsensitive(const QDir &dir, const QString &name)
    {
        if (QFileInfo(dir.filePath(name)).isFile())
            return true;

        // Linux filesystems are case-sensitive; retail/GOG copies vary.
        const QString lower = name.toLower();
        const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::NoSymLinks);
        for (const QFileInfo &e : entries) {
            if (e.fileName().toLower() == lower)
                return true;
        }
        return false;
    }

    static bool isAmnesiaInstall(const QString &path)
    {
        const QDir dir(path);
        if (!dir.exists())
            return false;

        const bool hasExe =
            fileExistsInsensitive(dir, "Amnesia.exe")
            || fileExistsInsensitive(dir, "amnesia-Win32-Release.exe")
            || fileExistsInsensitive(dir, "amnesia-x64-Release.exe");

        const bool hasCfg = fileExistsInsensitive(dir, "resources.cfg");
        const bool hasMaps = QDir(dir.filePath("maps")).exists()
            || QDir(dir.filePath("maps/main")).exists();

        // resources.cfg alone is too generic (other HPL games/mods match).
        return hasCfg && (hasExe || hasMaps);
    }

    static bool isSkippedDir(const QString &name)
    {
        static const QSet<QString> skip = {
            ".cache", "shadercache", "compatdata", "workshop",
            "pfx", "drive_c", "dosdevices", "windows",
            "prefix", "prefixes",
            "$RECYCLE.BIN", "System Volume Information"
        };
        if (skip.contains(name))
            return true;
        if (name.compare("Program Files", Qt::CaseInsensitive) == 0)
            return true;
        if (name.startsWith("Program Files (", Qt::CaseInsensitive))
            return true;
        return false;
    }

    void setBusy(bool busy, const QString &msg = {})
    {
        scanButton->setEnabled(!busy);
        browseButton->setEnabled(!busy);
        installButton->setEnabled(!busy && !selectedFolder.isEmpty());
        progressBar->setVisible(busy);
        if (!msg.isEmpty())
            statusLabel->setText(msg);
        QApplication::processEvents();
    }

    void selectFolder(const QString &path)
    {
        selectedFolder = QDir(path).absolutePath();
        folderLabel->setText("Selected: " + selectedFolder);

        QFileInfo info(selectedFolder);
        if (!info.isWritable()) {
            statusLabel->setText("Game folder selected, but it is not writable. "
                                 "Check permissions / Proton ownership.");
            installButton->setEnabled(false);
            return;
        }

        statusLabel->setText("Game folder selected.");
        installButton->setEnabled(true);

        QSettings settings("Atrim", "AtrimLinux");
        settings.setValue("lastFolder", selectedFolder);
    }

    void chooseFolder()
    {
        QSettings settings("Atrim", "AtrimLinux");
        const QString startDir = settings.value("lastFolder", QDir::homePath()).toString();

        const QString path = QFileDialog::getExistingDirectory(
            this,
            "Choose Amnesia game folder",
            startDir
        );

        if (path.isEmpty())
            return;

        if (!isAmnesiaInstall(path)) {
            const auto answer = QMessageBox::question(
                this,
                "Folder not recognized",
                "This folder was not confidently identified as an "
                "Amnesia installation (expected resources.cfg plus game exe/maps). "
                "Use it anyway?",
                QMessageBox::Yes | QMessageBox::No,
                QMessageBox::No
            );

            if (answer != QMessageBox::Yes)
                return;
        }

        selectFolder(path);
    }

    static QStringList steamLibraries()
    {
        const QString home = QDir::homePath();
        const QStringList steamRoots = {
            home + "/.steam/steam",
            home + "/.local/share/Steam",
            home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam"
        };

        QStringList libraries;
        // Matches both legacy and current libraryfolders.vdf quoting.
        const QRegularExpression pathRe(
            R"rx("path"\s+"([^"]+)")rx",
            QRegularExpression::CaseInsensitiveOption
        );

        for (const QString &steamRoot : steamRoots) {
            QFile file(steamRoot + "/steamapps/libraryfolders.vdf");
            if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
                continue;

            const QString text = QString::fromUtf8(file.readAll());
            auto matches = pathRe.globalMatch(text);
            while (matches.hasNext()) {
                QString library = matches.next().captured(1);
                library.replace("\\\\", "/");
                library.replace("\\", "/");
                libraries << QDir(library).filePath("steamapps/common");
            }
        }
        return libraries;
    }

    void scanLocations()
    {
        setBusy(true, "Scanning for Amnesia installations…");

        const QString home = QDir::homePath();

        QStringList roots = {
            home + "/.steam/steam/steamapps/common",
            home + "/.local/share/Steam/steamapps/common",
            home + "/.var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps/common",
            home + "/Games/Heroic",
            home + "/Games"
        };
        roots << steamLibraries();

        for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
            const QString mount = QDir::cleanPath(volume.rootPath());
            if (mount.startsWith("/mnt/")
                || mount.startsWith("/media/")
                || mount.startsWith("/run/media/")) {
                roots << mount;
            }
        }

        roots.removeDuplicates();
        // Drop non-existent roots so walk() does not waste time.
        roots.erase(std::remove_if(roots.begin(), roots.end(),
            [](const QString &r) { return !QDir(r).exists(); }),
            roots.end());

        QStringList found;
        QSet<QString> visited;
        int visitedCount = 0;
        constexpr int maxVisited = 20000;
        constexpr int maxDepth = 5;

        std::function<void(const QString &, int)> walk =
            [&](const QString &path, int depth) {
                if (visitedCount >= maxVisited || depth < 0)
                    return;

                const QString canonical =
                    QFileInfo(path).canonicalFilePath();

                if (canonical.isEmpty() || visited.contains(canonical))
                    return;

                visited.insert(canonical);

                QDir dir(canonical);
                if (!dir.exists())
                    return;

                ++visitedCount;
                if (visitedCount % 500 == 0)
                    QApplication::processEvents();

                if (isAmnesiaInstall(canonical)) {
                    found << canonical;
                    return;
                }

                if (depth == 0)
                    return;

                const QFileInfoList children = dir.entryInfoList(
                    QDir::Dirs
                    | QDir::NoDotAndDotDot
                    | QDir::Readable
                    | QDir::NoSymLinks,
                    QDir::Name
                );

                for (const QFileInfo &child : children) {
                    if (isSkippedDir(child.fileName()))
                        continue;

                    walk(child.absoluteFilePath(), depth - 1);

                    if (visitedCount >= maxVisited)
                        return;
                }
            };

        for (const QString &root : roots) {
            // External mounts: keep depth shallow to avoid full-disk walks.
            const bool isMount = root.startsWith("/mnt/")
                || root.startsWith("/media/")
                || root.startsWith("/run/media/");
            walk(root, isMount ? 4 : maxDepth);

            if (visitedCount >= maxVisited)
                break;
        }

        found.removeDuplicates();
        found.sort(Qt::CaseInsensitive);

        setBusy(false);

        if (found.isEmpty()) {
            statusLabel->setText(
                "No installation found. Choose the game folder manually."
            );
            return;
        }

        if (found.size() == 1) {
            selectFolder(found.first());
            return;
        }

        bool accepted = false;
        const QString selected = QInputDialog::getItem(
            this,
            "Amnesia installations found",
            "Choose the game folder:",
            found,
            0,
            false,
            &accepted
        );

        if (accepted && !selected.isEmpty())
            selectFolder(selected);
    }

    static bool runBlocking(const QString &program, const QStringList &args,
                            QString *stderrOut, int timeoutMs = -1)
    {
        QProcess proc;
        proc.setProgram(program);
        proc.setArguments(args);
        proc.start();
        if (!proc.waitForStarted(10000)) {
            if (stderrOut)
                *stderrOut = "Failed to start " + program + ": " + proc.errorString();
            return false;
        }
        if (!proc.waitForFinished(timeoutMs)) {
            proc.kill();
            proc.waitForFinished(5000);
            if (stderrOut)
                *stderrOut = program + " timed out.";
            return false;
        }
        if (stderrOut)
            *stderrOut = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        return proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    }

    static QStringList findAmnesiaSaveDirs()
    {
        QStringList out;
        const QString home = QDir::homePath();

        const QString nativeDoc = home + "/Documents/Amnesia";
        if (QFileInfo(nativeDoc).isDir())
            out << nativeDoc;

        // Heroic Proton prefixes: <base>/*/pfx/drive_c/users/*/Documents/Amnesia
        const QStringList prefixBases = {
            home + "/Games/Heroic/Prefixes",
            home + "/.config/heroic/prefixes"
        };
        for (const QString &base : prefixBases) {
            QDir baseDir(base);
            if (!baseDir.exists())
                continue;
            const QFileInfoList prefixes = baseDir.entryInfoList(
                QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
            for (const QFileInfo &prefix : prefixes) {
                QDir usersDir(prefix.absoluteFilePath() + "/pfx/drive_c/users");
                if (!usersDir.exists())
                    continue;
                const QFileInfoList users = usersDir.entryInfoList(
                    QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
                for (const QFileInfo &user : users) {
                    const QString save = user.absoluteFilePath() + "/Documents/Amnesia";
                    if (QFileInfo(save).isDir())
                        out << save;
                }
            }
        }
        out.removeDuplicates();
        return out;
    }

    void installSelected()
    {
        if (selectedFolder.isEmpty()) {
            QMessageBox::warning(
                this,
                "No game folder",
                "Choose or scan for an Amnesia game folder first."
            );
            return;
        }

        if (!QFileInfo(selectedFolder).isWritable()) {
            QMessageBox::warning(
                this,
                "Folder not writable",
                "The selected game folder is not writable:\n\n" + selectedFolder
            );
            return;
        }

        for (const char *tool : {"curl", "unzip", "cp"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty()) {
                QMessageBox::critical(
                    this,
                    "Missing dependency",
                    QString("Required tool '%1' was not found in PATH.\n"
                            "Install it with your package manager and retry.").arg(tool)
                );
                return;
            }
        }

        const QString url = versionBox->currentData().toString();
        const QString version = versionBox->currentText();

        const auto answer = QMessageBox::question(
            this,
            "Confirm installation",
            "Install " + version + " into:\n\n"
            + selectedFolder
            + "\n\nExisting files will be backed up first.",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );

        if (answer != QMessageBox::Yes)
            return;

        setBusy(true, "Downloading " + version + "…");

        QTemporaryDir tempDir;
        if (!tempDir.isValid()) {
            setBusy(false);
            QMessageBox::critical(this, "Install failed", "Could not create temp directory.");
            return;
        }

        const QString archive = tempDir.filePath("atrim.zip");
        const QString extracted = tempDir.filePath("extracted");

        QString err;
        if (!runBlocking("curl", {"--fail", "--location", "--retry", "3",
                                  "--connect-timeout", "30",
                                  "--silent", "--show-error",
                                  "--output", archive, url}, &err)) {
            setBusy(false);
            QMessageBox::critical(this, "Download failed",
                err.isEmpty() ? "curl failed with no error output." : err);
            return;
        }

        const qint64 archiveSize = QFileInfo(archive).size();
        if (archiveSize < 100 * 1024) {
            setBusy(false);
            QMessageBox::critical(this, "Download failed",
                QString("Downloaded file is suspiciously small (%1 bytes). Aborting.").arg(archiveSize));
            return;
        }

        statusLabel->setText("Extracting archive…");
        QApplication::processEvents();
        QDir().mkpath(extracted);

        if (!runBlocking("unzip", {"-q", archive, "-d", extracted}, &err)) {
            setBusy(false);
            QMessageBox::critical(this, "Extraction failed",
                err.isEmpty() ? "unzip failed with no error output." : err);
            return;
        }

        // Validate payload before touching the game folder.
        bool payloadOk = false;
        {
            QDirIterator it(extracted, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                const QString name = it.next().toLower();
                if (name.endsWith("amnesia.exe")
                    || name.endsWith("amnesia-win32-release.exe")
                    || name.endsWith("amnesia-x64-release.exe")
                    || name.endsWith("resources.cfg")
                    || name.endsWith("sdl.dll")) {
                    payloadOk = true;
                    break;
                }
            }
        }
        if (!payloadOk) {
            setBusy(false);
            QMessageBox::critical(this, "Install failed",
                "Archive did not contain expected Amnesia/Atrim files. Aborting before modifying the game folder.");
            return;
        }

        const QString backupRoot =
            QDir::homePath()
            + "/.local/share/atrim-backups/"
            + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");

        if (!QDir().mkpath(backupRoot)) {
            setBusy(false);
            QMessageBox::critical(this, "Backup failed",
                "Could not create backup directory:\n" + backupRoot);
            return;
        }

        // Back up any saves found in known locations.
        statusLabel->setText("Backing up saves and game files…");
        QApplication::processEvents();

        int saveIndex = 0;
        for (const QString &saveSource : findAmnesiaSaveDirs()) {
            const QString dest = backupRoot + QString("/Amnesia-saves-%1").arg(++saveIndex);
            if (!runBlocking("cp", {"-a", saveSource, dest}, &err)) {
                setBusy(false);
                QMessageBox::critical(this, "Save backup failed", err);
                return;
            }
        }

        // Back up exactly the files this archive would overwrite.
        QDir extractedDir(extracted);
        const QFileInfoList topLevel = extractedDir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : topLevel) {
            const QString target = selectedFolder + "/" + entry.fileName();
            if (!QFileInfo::exists(target))
                continue;
            if (!runBlocking("cp", {"-a", target, backupRoot + "/" + entry.fileName()}, &err)) {
                setBusy(false);
                QMessageBox::critical(this, "Backup failed",
                    "Could not back up " + target + ":\n" + err);
                return;
            }
        }

        statusLabel->setText("Installing files…");
        QApplication::processEvents();

        if (!runBlocking("cp", {"-a", extracted + "/.", selectedFolder + "/"}, &err)) {
            setBusy(false);
            QMessageBox::critical(this, "Installation failed", err);
            return;
        }

        setBusy(false);
        statusLabel->setText("Installation complete.\nBackup saved at:\n" + backupRoot);

        QMessageBox::information(
            this,
            "Atrim installed",
            "Atrim was installed successfully.\n\nBackup saved at:\n" + backupRoot +
            "\n\nOn Linux, launch the Windows executable through Proton or "
            "another compatible Windows runtime."
        );
    }

    static bool parseGLVersion(const QString &s, int &major, int &minor)
    {
        const QRegularExpression re(R"((\d+)\.(\d+))");
        const auto m = re.match(s);
        if (!m.hasMatch())
            return false;
        major = m.captured(1).toInt();
        minor = m.captured(2).toInt();
        return true;
    }

    void checkOpenGL()
    {
        QSurfaceFormat requested;
        requested.setRenderableType(QSurfaceFormat::OpenGL);
        requested.setVersion(2, 1);

        QOpenGLContext context;
        context.setFormat(requested);

        if (!context.create()) {
            statusLabel->setText("Could not create an OpenGL context.");
            return;
        }

        QOffscreenSurface surface;
        surface.setFormat(context.format());
        surface.create();

        if (!surface.isValid()) {
            statusLabel->setText("Could not create an offscreen OpenGL surface.");
            return;
        }

        if (!context.makeCurrent(&surface)) {
            statusLabel->setText("Could not make the OpenGL context current.");
            return;
        }

        QOpenGLFunctions *gl = context.functions();
        if (!gl) {
            context.doneCurrent();
            statusLabel->setText("Could not get OpenGL functions.");
            return;
        }

        gl->initializeOpenGLFunctions();

        const auto getString = [gl](GLenum name) {
            const GLubyte *value = gl->glGetString(name);
            return value
                ? QString::fromLatin1(reinterpret_cast<const char *>(value))
                : QStringLiteral("Unavailable");
        };

        const QString vendor = getString(GL_VENDOR);
        const QString renderer = getString(GL_RENDERER);
        const QString versionStr = getString(GL_VERSION);
        context.doneCurrent();

        int major = 0, minor = 0;
        const bool parsed = parseGLVersion(versionStr, major, minor);
        const bool enough = parsed && (major > 2 || (major == 2 && minor >= 1));
        const bool software = renderer.contains("llvmpipe", Qt::CaseInsensitive)
            || renderer.contains("softpipe", Qt::CaseInsensitive)
            || renderer.contains("software", Qt::CaseInsensitive)
            || renderer.contains("swrast", Qt::CaseInsensitive);

        QString details =
            (enough ? "OpenGL OK (needs 2.1+)." : "OpenGL TOO OLD (needs 2.1+).")
            + QString("\nVendor: " + vendor)
            + QString("\nRenderer: " + renderer)
            + QString("\nVersion: " + versionStr);
        if (software)
            details += "\nNote: software rendering detected — expect poor performance.";
        if (!parsed)
            details += "\nNote: could not parse GL version.";

        statusLabel->setText(details);
    }
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    Launcher window;
    window.show();

    return app.exec();
}
