#include <QApplication>
#include <QComboBox>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QIODevice>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
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
#include <QTimer>
#include <QUrl>
#include <QDesktopServices>
#include <QDateTime>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <functional>

// Portable across init systems (systemd / OpenRC / runit / s6) and distros
// (glibc / musl, GNU / BusyBox): no systemctl/logind/journald dependency,
// no hard requirement on curl, no GNU-only cp flags. Downloads use QtNetwork,
// file copies use Qt, extraction tries whatever backend the distro provides.

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

        installButton = new QPushButton("Install selected Atrim version");
        installButton->setEnabled(false);
        installButton->setToolTip("Download and install the selected version into the game folder");

        restoreButton = new QPushButton("Restore from backup…");
        restoreButton->setToolTip("Restore files from a previous Atrim backup");

        auto *glButton = new QPushButton("Check OpenGL");
        glButton->setToolTip("Amnesia needs OpenGL 2.1 or newer");

        cancelButton = new QPushButton("Cancel download");
        cancelButton->setVisible(false);

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
        layout->addWidget(restoreButton);
        layout->addWidget(glButton);
        layout->addWidget(cancelButton);
        layout->addWidget(progressBar);
        layout->addWidget(statusLabel);
        layout->addStretch(1);

        connect(scanButton, &QPushButton::clicked,
                this, [this] { scanLocations(); });

        connect(browseButton, &QPushButton::clicked,
                this, [this] { chooseFolder(); });

        connect(installButton, &QPushButton::clicked,
                this, [this] { installSelected(); });

        connect(restoreButton, &QPushButton::clicked,
                this, [this] { restoreBackup(); });

        connect(glButton, &QPushButton::clicked,
                this, [this] { checkOpenGL(); });

        connect(cancelButton, &QPushButton::clicked, this, [this] {
            if (activeReply)
                activeReply->abort();
        });

        // Restore last folder if it is still valid.
        QSettings settings("Atrim", "AtrimLinux");
        const QString last = settings.value("lastFolder").toString();
        if (!last.isEmpty() && isAmnesiaInstall(last))
            selectFolder(last);

        populateVersions();
    }

private:
    QLabel *folderLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QComboBox *versionBox = nullptr;
    QPushButton *installButton = nullptr;
    QPushButton *scanButton = nullptr;
    QPushButton *browseButton = nullptr;
    QPushButton *restoreButton = nullptr;
    QPushButton *cancelButton = nullptr;
    QProgressBar *progressBar = nullptr;
    QNetworkReply *activeReply = nullptr;
    QString selectedFolder;

    struct ReleaseInfo {
        QString label;
        QString url;
        qint64 size = 0;
        QString sha256;
    };

    static bool fileExistsInsensitive(const QDir &dir, const QString &name)
    {
        if (QFileInfo(dir.filePath(name)).isFile())
            return true;

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
        restoreButton->setEnabled(!busy);
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
        roots.erase(std::remove_if(roots.begin(), roots.end(),
            [](const QString &r) { return !QDir(r).exists(); }),
            roots.end());

        QStringList found;
        QSet<QString> visited;
        int visitedCount = 0;
        constexpr int maxVisited = 20000;
        constexpr int maxDepth = 5;
        bool hitLimit = false;

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

                    if (visitedCount >= maxVisited) {
                        hitLimit = true;
                        return;
                    }
                }
            };

        for (const QString &root : roots) {
            const bool isMount = root.startsWith("/mnt/")
                || root.startsWith("/media/")
                || root.startsWith("/run/media/");
            walk(root, isMount ? 4 : maxDepth);

            if (visitedCount >= maxVisited) {
                hitLimit = true;
                break;
            }
        }

        found.removeDuplicates();
        found.sort(Qt::CaseInsensitive);

        setBusy(false);

        if (found.isEmpty()) {
            statusLabel->setText(
                hitLimit
                    ? "No installation found (scan limit reached). Choose the game folder manually."
                    : "No installation found. Choose the game folder manually."
            );
            return;
        }

        if (found.size() == 1) {
            selectFolder(found.first());
            if (hitLimit)
                statusLabel->setText(statusLabel->text()
                    + "\n(Scan limit reached; other installs may exist.)");
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

    bool downloadFile(const QUrl &url, const QString &destPath, QString *errOut)
    {
        QFile out(destPath);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (errOut)
                *errOut = "Cannot write to " + destPath + ": " + out.errorString();
            return false;
        }

        QNetworkAccessManager manager;
        QNetworkRequest request(url);
        request.setHeader(QNetworkRequest::UserAgentHeader, "AtrimLinuxLauncher/0.3");
#if QT_VERSION_MAJOR >= 6
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
#else
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif

        QNetworkReply *reply = manager.get(request);
        if (!reply) {
            if (errOut)
                *errOut = "Network request failed to start.";
            return false;
        }
        activeReply = reply;
        cancelButton->setVisible(true);

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(180000);
        connect(&timeout, &QTimer::timeout, &loop, [&] { reply->abort(); });
        connect(reply, &QNetworkReply::downloadProgress, this,
                [&](qint64 received, qint64 total) {
            if (total > 0) {
                progressBar->setRange(0, 100);
                progressBar->setValue(static_cast<int>(received * 100 / total));
                statusLabel->setText(
                    QString("Downloading… %1 / %2 MiB")
                    .arg(received / 1048576).arg(total / 1048576));
                QApplication::processEvents();
            }
        });
        connect(reply, &QNetworkReply::readyRead, &loop, [&] {
            out.write(reply->readAll());
        });
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start();
        loop.exec();
        timeout.stop();

        activeReply = nullptr;
        cancelButton->setVisible(false);

        const bool ok = (reply->error() == QNetworkReply::NoError);
        QString netErr;
        if (!ok)
            netErr = reply->errorString();
        out.write(reply->readAll());
        out.close();
        reply->deleteLater();

        if (!ok) {
            QFile::remove(destPath);
            if (errOut) {
                if (netErr.contains(QStringLiteral("cancel"), Qt::CaseInsensitive)
                    || netErr.contains(QStringLiteral("Operation canceled"), Qt::CaseInsensitive))
                    *errOut = "Download canceled.";
                else
                    *errOut = "Download failed: " + netErr;
            }
            return false;
        }
        return true;
    }

    static bool copyFileOverwrite(const QString &src, const QString &dst, QString *errOut)
    {
        QFileInfo srcInfo(src);
        if (srcInfo.isSymLink()) {
            const QString target = srcInfo.symLinkTarget();
            QFile::remove(dst);
            if (QFile::link(target, dst))
                return true;
        }
        QDir().mkpath(QFileInfo(dst).absolutePath());
        QFile::remove(dst);
        if (!QFile::copy(src, dst)) {
            if (errOut)
                *errOut = "Copy failed: " + src + " -> " + dst;
            return false;
        }
        return true;
    }

    static bool copyRecursively(const QString &src, const QString &dst, QString *errOut)
    {
        QFileInfo info(src);
        if (info.isFile() || info.isSymLink())
            return copyFileOverwrite(src, dst, errOut);

        QDir srcDir(src);
        if (!srcDir.exists()) {
            if (errOut)
                *errOut = "Source does not exist: " + src;
            return false;
        }
        if (!QDir().mkpath(dst)) {
            if (errOut)
                *errOut = "Cannot create directory: " + dst;
            return false;
        }
        const QFileInfoList entries = srcDir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::Hidden | QDir::System | QDir::NoDotAndDotDot);
        for (const QFileInfo &e : entries) {
            if (!copyRecursively(e.absoluteFilePath(), dst + "/" + e.fileName(), errOut))
                return false;
        }
        return true;
    }

    static bool extractArchive(const QString &archive, const QString &destDir, QString *errOut)
    {
        QStringList attempts;
        QString lastErr;

        auto tryRun = [&](const QString &prog, const QStringList &args) -> bool {
            if (QStandardPaths::findExecutable(prog).isEmpty()) {
                attempts << prog + " (not installed)";
                return false;
            }
            QString err;
            if (runBlocking(prog, args, &err)) {
                QDir d(destDir);
                if (!d.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).isEmpty())
                    return true;
                lastErr = prog + " ran but produced no files.";
                return false;
            }
            lastErr = prog + ": " + err;
            attempts << (prog + ": " + err);
            return false;
        };

        if (tryRun("unzip", {"-q", "-o", archive, "-d", destDir}))
            return true;
        for (const char *seven : {"7z", "7zz", "7za"}) {
            if (tryRun(seven, {"x", archive, "-o" + destDir, "-y"}))
                return true;
        }
        if (tryRun("bsdtar", {"-xf", archive, "-C", destDir}))
            return true;
        for (const char *py : {"python3", "python"}) {
            if (tryRun(py, {"-m", "zipfile", "-e", archive, destDir}))
                return true;
        }

        if (errOut) {
            *errOut = "No usable unzip backend found.\nTried: " + attempts.join("; ")
                + (lastErr.isEmpty() ? "" : "\nLast error: " + lastErr)
                + "\nInstall one of: unzip, p7zip, bsdtar, or python3.\n"
                  "(apt: sudo apt install unzip | dnf: sudo dnf install unzip p7zip | "
                  "pacman: sudo pacman -S unzip p7zip | apk: sudo apk add unzip p7zip libarchive-tools python3 | "
                  "emerge: sudo emerge -av unzip p7zip libarchive | xbps: sudo xbps-install -S unzip p7zip bsdtar python3 | "
                  "zypper: sudo zypper install unzip p7zip)";
        }
        return false;
    }

    static QString backupBaseDir()
    {
        QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (base.isEmpty())
            base = QDir::homePath() + "/.local/share";
        return base + "/atrim-backups";
    }

    static QStringList findAmnesiaSaveDirs()
    {
        QStringList out;
        const QString home = QDir::homePath();

        const QString nativeDoc = home + "/Documents/Amnesia";
        if (QFileInfo(nativeDoc).isDir())
            out << nativeDoc;

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

    // Pinned offline fallback (official roozerxc/atrim Releases).
    static QList<ReleaseInfo> staticReleases()
    {
        return {
            {"Stable — v1.4.5-win32-release",
             "https://github.com/roozerxc/atrim/releases/download/"
             "v1.4.5-win32/v1.4.5-win32-release.zip",
             9082247,
             "377bb069185033ee5ae4a36017154a53532c688a19d13b4a9950dd7204fe7c91"},
            {"Beta — v1.4.6-win32-beta",
             "https://github.com/roozerxc/atrim/releases/download/"
             "v1.4.6-win32-beta/v1.4.6-win32-beta.zip",
             9311694,
             "f09142777bfb969f65b9f0fccd497199192bc52b206a5e18eb7339fce5d1d289"},
            {"Beta — v1.4.6-win64-beta",
             "https://github.com/roozerxc/atrim/releases/download/"
             "v1.4.6-win32-beta/v1.4.6-win64-beta.zip",
             10001867,
             "6367efd32dd3fdb233bc62bb6e2e390da176c42aa532913a9760e34ea3440e34"},
        };
    }

    static QStringList releaseToData(const ReleaseInfo &rel)
    {
        return {rel.url, QString::number(rel.size), rel.sha256};
    }

    QList<ReleaseInfo> fetchReleasesFromApi()
    {
        QList<ReleaseInfo> out;

        QNetworkAccessManager manager;
        QNetworkRequest request(QUrl(
            QStringLiteral("https://api.github.com/repos/roozerxc/atrim/releases?per_page=15")));
        request.setHeader(QNetworkRequest::UserAgentHeader, "AtrimLinuxLauncher/0.3");
        request.setRawHeader("Accept", "application/vnd.github+json");
#if QT_VERSION_MAJOR >= 6
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                             QNetworkRequest::NoLessSafeRedirectPolicy);
#else
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
#endif

        QNetworkReply *reply = manager.get(request);
        if (!reply)
            return out;

        QEventLoop loop;
        QTimer timeout;
        timeout.setSingleShot(true);
        timeout.setInterval(15000);
        connect(&timeout, &QTimer::timeout, &loop, [&] { reply->abort(); });
        connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        timeout.start();
        loop.exec();
        timeout.stop();

        if (reply->error() != QNetworkReply::NoError) {
            reply->deleteLater();
            return out;
        }

        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
        reply->deleteLater();
        if (!doc.isArray())
            return out;

        const QJsonArray releases = doc.array();
        for (const QJsonValue &rv : releases) {
            const QJsonObject ro = rv.toObject();
            const QString tag = ro.value(QStringLiteral("tag_name")).toString();
            const bool prerelease = ro.value(QStringLiteral("prerelease")).toBool();
            const QJsonArray assets = ro.value(QStringLiteral("assets")).toArray();
            for (const QJsonValue &av : assets) {
                const QJsonObject ao = av.toObject();
                const QString name = ao.value(QStringLiteral("name")).toString();
                if (!name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
                    continue;
                // Prefer known Atrim payload names; skip source archives.
                if (name.contains(QStringLiteral("source"), Qt::CaseInsensitive))
                    continue;

                ReleaseInfo rel;
                rel.url = ao.value(QStringLiteral("browser_download_url")).toString();
                rel.size = static_cast<qint64>(ao.value(QStringLiteral("size")).toDouble());
                // GitHub API does not publish SHA-256 of assets; leave empty
                // unless we match a known static pin below.
                rel.sha256.clear();
                rel.label = (prerelease ? QStringLiteral("Beta — ") : QStringLiteral("Stable — "))
                    + (tag.isEmpty() ? name : tag + " / " + name);

                if (rel.url.isEmpty())
                    continue;

                // Overlay pinned SHA-256 when URL matches a static entry.
                for (const ReleaseInfo &pin : staticReleases()) {
                    if (pin.url == rel.url) {
                        rel.sha256 = pin.sha256;
                        if (rel.size <= 0)
                            rel.size = pin.size;
                        break;
                    }
                }
                out << rel;
            }
        }
        return out;
    }

    void populateVersions()
    {
        versionBox->clear();
        statusLabel->setText("Fetching release list…");
        QApplication::processEvents();

        QList<ReleaseInfo> list = fetchReleasesFromApi();
        QString sourceNote;
        if (list.isEmpty()) {
            list = staticReleases();
            sourceNote = "Using offline release list (GitHub API unavailable).";
        } else {
            sourceNote = QString("Loaded %1 asset(s) from GitHub.").arg(list.size());
        }

        for (const ReleaseInfo &rel : list)
            versionBox->addItem(rel.label, releaseToData(rel));

        QSettings settings("Atrim", "AtrimLinux");
        const QString lastVer = settings.value("lastVersion").toString();
        if (!lastVer.isEmpty()) {
            const int idx = versionBox->findText(lastVer);
            if (idx >= 0)
                versionBox->setCurrentIndex(idx);
        }

        statusLabel->setText(sourceNote);
    }

    ReleaseInfo currentRelease() const
    {
        ReleaseInfo rel;
        rel.label = versionBox->currentText();
        const QStringList data = versionBox->currentData().toStringList();
        if (data.size() > 0)
            rel.url = data.at(0);
        if (data.size() > 1)
            rel.size = data.at(1).toLongLong();
        if (data.size() > 2)
            rel.sha256 = data.at(2);
        return rel;
    }

    static QString sha256OfFile(const QString &path, QString *errOut)
    {
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            if (errOut)
                *errOut = "Cannot read " + path + ": " + f.errorString();
            return {};
        }
        QCryptographicHash hash(QCryptographicHash::Sha256);
        while (!f.atEnd())
            hash.addData(f.read(1 << 20));
        return QString::fromLatin1(hash.result().toHex());
    }

    void restoreBackup()
    {
        const QString base = backupBaseDir();
        QDir baseDir(base);
        if (!baseDir.exists()) {
            QMessageBox::information(this, "No backups",
                "No backups found in:\n" + base);
            return;
        }

        QStringList stamps = baseDir.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        if (stamps.isEmpty()) {
            QMessageBox::information(this, "No backups",
                "No backups found in:\n" + base);
            return;
        }

        bool ok = false;
        const QString stamp = QInputDialog::getItem(
            this, "Restore backup", "Choose backup:", stamps, 0, false, &ok);
        if (!ok || stamp.isEmpty())
            return;

        if (selectedFolder.isEmpty()) {
            QMessageBox::warning(this, "No game folder",
                "Select the Amnesia game folder first, then restore into it.");
            return;
        }

        const QString backupPath = baseDir.filePath(stamp);
        const auto confirm = QMessageBox::question(
            this, "Confirm restore",
            "Restore from:\n" + backupPath + "\n\nInto:\n" + selectedFolder
            + "\n\nFiles with the same names will be overwritten.",
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (confirm != QMessageBox::Yes)
            return;

        setBusy(true, "Restoring backup…");
        QString err;
        QDir bak(backupPath);
        const QFileInfoList entries = bak.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &e : entries) {
            if (!copyRecursively(e.absoluteFilePath(),
                                 selectedFolder + "/" + e.fileName(), &err)) {
                setBusy(false);
                QMessageBox::critical(this, "Restore failed", err);
                return;
            }
        }
        setBusy(false);
        statusLabel->setText("Restore complete from:\n" + backupPath);
        QMessageBox::information(this, "Restored",
            "Backup restored into:\n" + selectedFolder);
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

        const ReleaseInfo rel = currentRelease();
        const QString url = rel.url;
        const QString version = rel.label;

        if (url.isEmpty()) {
            QMessageBox::critical(this, "No download URL",
                "No download URL is configured for this version.");
            return;
        }

        {
            QStorageInfo storage(selectedFolder);
            storage.refresh();
            const qint64 need = qMax(rel.size, qint64(20) * 1024 * 1024) * 3;
            if (storage.isValid() && storage.bytesAvailable() > 0
                && storage.bytesAvailable() < need) {
                QMessageBox::warning(
                    this, "Not enough free space",
                    QString("Need about %1 MiB free near the game folder.\nAvailable: %2 MiB.")
                        .arg(need / 1048576)
                        .arg(storage.bytesAvailable() / 1048576));
                return;
            }
        }

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

        QSettings settings("Atrim", "AtrimLinux");
        settings.setValue("lastVersion", version);

        setBusy(true, "Downloading " + version + "…");
        progressBar->setRange(0, 0);

        QTemporaryDir tempDir;
        if (!tempDir.isValid()) {
            setBusy(false);
            QMessageBox::critical(this, "Install failed", "Could not create temp directory.");
            return;
        }

        const QString archive = tempDir.filePath("atrim.zip");
        const QString extracted = tempDir.filePath("extracted");

        QString err;
        if (!downloadFile(QUrl(url), archive, &err)) {
            setBusy(false);
            progressBar->setRange(0, 0);
            QMessageBox::critical(this, "Download failed",
                err.isEmpty() ? "Download failed with no error output." : err);
            return;
        }

        const qint64 archiveSize = QFileInfo(archive).size();
        if (archiveSize < 100 * 1024) {
            setBusy(false);
            QMessageBox::critical(this, "Download failed",
                QString("Downloaded file is suspiciously small (%1 bytes). Aborting.").arg(archiveSize));
            return;
        }

        if (rel.size > 0 && archiveSize != rel.size) {
            setBusy(false);
            QMessageBox::critical(this, "Download failed",
                QString("Size mismatch: got %1 bytes, expected %2 bytes.\n"
                        "The release asset may have changed. Aborting.")
                    .arg(archiveSize).arg(rel.size));
            return;
        }

        if (!rel.sha256.isEmpty()) {
            statusLabel->setText("Verifying download…");
            QApplication::processEvents();
            const QString actual = sha256OfFile(archive, &err);
            if (actual.isEmpty()) {
                setBusy(false);
                QMessageBox::critical(this, "Verification failed", err);
                return;
            }
            if (actual.compare(rel.sha256, Qt::CaseInsensitive) != 0) {
                setBusy(false);
                const auto cont = QMessageBox::warning(
                    this,
                    "Checksum mismatch",
                    "SHA-256 does not match the official release asset.\n\n"
                    "Expected:\n" + rel.sha256 + "\n\nGot:\n" + actual +
                    "\n\nContinue anyway?",
                    QMessageBox::Yes | QMessageBox::No,
                    QMessageBox::No
                );
                if (cont != QMessageBox::Yes)
                    return;
                setBusy(true, "Installing " + version + "…");
            }
        }

        statusLabel->setText("Extracting archive…");
        progressBar->setRange(0, 0);
        QApplication::processEvents();
        QDir().mkpath(extracted);

        if (!extractArchive(archive, extracted, &err)) {
            setBusy(false);
            QMessageBox::critical(this, "Extraction failed",
                err.isEmpty() ? "Extraction failed with no error output." : err);
            return;
        }

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
            backupBaseDir() + "/"
            + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss");

        if (!QDir().mkpath(backupRoot)) {
            setBusy(false);
            QMessageBox::critical(this, "Backup failed",
                "Could not create backup directory:\n" + backupRoot);
            return;
        }

        statusLabel->setText("Backing up saves and game files…");
        QApplication::processEvents();

        int saveIndex = 0;
        for (const QString &saveSource : findAmnesiaSaveDirs()) {
            const QString dest = backupRoot + QString("/Amnesia-saves-%1").arg(++saveIndex);
            if (!copyRecursively(saveSource, dest, &err)) {
                setBusy(false);
                QMessageBox::critical(this, "Save backup failed", err);
                return;
            }
        }

        QDir extractedDir(extracted);
        const QFileInfoList topLevel = extractedDir.entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &entry : topLevel) {
            const QString target = selectedFolder + "/" + entry.fileName();
            if (!QFileInfo::exists(target))
                continue;
            if (!copyRecursively(target, backupRoot + "/" + entry.fileName(), &err)) {
                setBusy(false);
                QMessageBox::critical(this, "Backup failed",
                    "Could not back up " + target + ":\n" + err);
                return;
            }
        }

        statusLabel->setText("Installing files…");
        QApplication::processEvents();

        for (const QFileInfo &entry : topLevel) {
            if (!copyRecursively(entry.absoluteFilePath(),
                                 selectedFolder + "/" + entry.fileName(), &err)) {
                setBusy(false);
                QMessageBox::critical(this, "Installation failed", err);
                return;
            }
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