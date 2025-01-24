#include <wiisocket.h>
#include <stdio.h>
#include <curl/curl.h>
#include <mbedtls/sha1.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gctypes.h>
#include <gccore.h>
#include <fat.h>
#include <malloc.h>
#include <ogcsys.h>
#include <ogc/lwp_watchdog.h>
#include <sys/stat.h>
#include <sdcard/wiisd_io.h>
#include <dirent.h>
#include <errno.h>
#include <filesystem>
#include <vector>
#include "update.h"
#include "haxx.h"
#include "menu.h"
#include "launcher.h"
#include "riivolution_config.h"
#include "init.h"
#include "files.h"
#include "unzip.h"

using std::vector;
namespace fs = std::filesystem;

#define MAX_URL_LENGTH 256
#define MAX_FILENAME_LENGTH 128
#define MAX_DIRECTORY_LENGTH 128
#define MAX_VERSION_LENGTH 16
#define MAX_PATH_LENGTH 256
#define MAX_FIELD_LENGTH 112

// Global variables for progress and versioning
static double totalSize = 0;
static double downloadedSize = 0;
static bool done2 = false;
static bool die2 = false;
static bool initfat = false;

int filesToDownload = 0;
int filesDownloaded = 0;

// Forward declarations
void extractZip(const char *zipFilename, const char *outputDirectory);
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp);

// Callback for writing downloaded data to file
size_t WriteCallback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    FILE *fp = static_cast<FILE*>(userp);
    return fwrite(contents, size, nmemb, fp);
}

// Compares two version strings. Returns -1, 0, or 1.
int compareVersions(const char *localVersion, const char *onlineVersion) {
    int localMajor, localMinor, localPatch;
    int onlineMajor, onlineMinor, onlinePatch;
    sscanf(localVersion, "%d.%d.%d", &localMajor, &localMinor, &localPatch);
    sscanf(onlineVersion, "%d.%d.%d", &onlineMajor, &onlineMinor, &onlinePatch);

    if (localMajor != onlineMajor)
        return (localMajor < onlineMajor) ? -1 : 1;
    if (localMinor != onlineMinor)
        return (localMinor < onlineMinor) ? -1 : 1;
    if (localPatch != onlinePatch)
        return (localPatch < onlinePatch) ? -1 : 1;
    return 0;
}

// Progress callback for libcurl
int ProgressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    if (dltotal <= 0) return 0; // Avoid division by zero

    double fileProgress = (dlnow / dltotal) * 100.0;
    double overallProgress = (((double)filesDownloaded / filesToDownload) * 100.0) + (fileProgress / filesToDownload);

    // Ensure progress reaches 100% on final file completion
    if (dlnow == dltotal && filesDownloaded == filesToDownload - 1) {
        overallProgress = 100.0;
    }

    char progressText[40];
    snprintf(progressText, sizeof(progressText), "%.0f%%", overallProgress);
    // Assuming ProgressText is a valid UI text element; update accordingly in your environment
    ProgressText->SetText(progressText);

    return 0;
}

// Extracts the given zip file to the specified output directory
void extractZip(const char *zipFilename, const char *outputDirectory) {
    unzFile zipFile = unzOpen(zipFilename);
    if (!zipFile) {
        printf("Error opening zip file %s\n", zipFilename);
        return;
    }

    // Count files in the zip for progress tracking
    int totalFilesInZip = 0;
    int status = unzGoToFirstFile(zipFile);
    while (status == UNZ_OK) {
        totalFilesInZip++;
        status = unzGoToNextFile(zipFile);
    }

    if (unzGoToFirstFile(zipFile) != UNZ_OK) {
        printf("Failed to go to first file in zip\n");
        unzClose(zipFile);
        return;
    }

    // Save current working directory and switch to output directory
    char originalCwd[MAX_PATH_LENGTH];
    getcwd(originalCwd, sizeof(originalCwd));
    if (chdir(outputDirectory) != 0) {
        printf("Failed to change directory to %s\n", outputDirectory);
        unzClose(zipFile);
        return;
    }

    int filesExtracted = 0;
    do {
        char filename[MAX_FILENAME_LENGTH] = {0};
        unz_file_info file_info;
        if (unzGetCurrentFileInfo(zipFile, &file_info, filename, sizeof(filename), NULL, 0, NULL, 0) != UNZ_OK) {
            printf("Failed to get file info\n");
            continue;
        }

        if (unzOpenCurrentFile(zipFile) != UNZ_OK) {
            printf("Failed to open file %s in zip\n", filename);
            continue;
        }

        // Create necessary directories
        const char *lastSlash = strrchr(filename, '/');
        if (lastSlash) {
            std::string directoryPath(filename, lastSlash - filename);
            if (!fs::exists(directoryPath)) {
                fs::create_directories(directoryPath);
            }
        }

        FILE *fp = fopen(filename, "wb");
        if (!fp) {
            printf("Failed to open file %s for writing\n", filename);
            unzCloseCurrentFile(zipFile);
            continue;
        }

        char buffer[4096];
        int readBytes;
        while ((readBytes = unzReadCurrentFile(zipFile, buffer, sizeof(buffer))) > 0) {
            fwrite(buffer, 1, readBytes, fp);
        }

        fclose(fp);
        unzCloseCurrentFile(zipFile);
        printf("Extracted: %s\n", filename);

        filesExtracted++;
        double progress = ((double)filesExtracted / totalFilesInZip) * 100.0;
        printf("Extraction Progress: %.2f%%\n", progress);
    } while (unzGoToNextFile(zipFile) == UNZ_OK);

    unzClose(zipFile);
    chdir(originalCwd);
}

// Downloads a file from the given URL to the specified local output filename.
// If the file is a zip, it will be extracted and then deleted.
void downloadFile(const char *url, const char *outputFilename) {
    CURL *curl = nullptr;
    CURLcode res;
    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();
    if (!curl) {
        curl_global_cleanup();
        return;
    }

    // Prepare directory for output file
    std::string outputPath(outputFilename);
    size_t lastSlashIdx = outputPath.rfind('/');
    if (lastSlashIdx != std::string::npos) {
        std::string directoryPath = outputPath.substr(0, lastSlashIdx);
        if (!fs::exists(directoryPath)) {
            fs::create_directories(directoryPath);
        }
    }

    FILE *fp = fopen(outputFilename, "wb");
    if (!fp) {
        printf("Error opening file %s for writing.\n", outputFilename);
        curl_easy_cleanup(curl);
        curl_global_cleanup();
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, ProgressCallback);

    // Reset file-specific progress counters
    totalSize = 0;
    downloadedSize = 0;

    res = curl_easy_perform(curl);
    fclose(fp);

    if (res != CURLE_OK) {
        fprintf(stderr, "curl_easy_perform() failed for %s: %s\n", outputFilename, curl_easy_strerror(res));
    } else {
        printf("Downloaded: %s\n", outputFilename);
        // If a zip file, extract and remove it
        if (strstr(outputFilename, ".zip") != nullptr) {
            // Determine extraction directory: use directory of the zip file
            std::string outputDir = outputPath.substr(0, lastSlashIdx);
            extractZip(outputFilename, outputDir.c_str());
            remove(outputFilename);
        }
        filesDownloaded++;
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
}

// Updates the local version file with the new version string.
void updateLocalVersion(const char *newVersion) {
    FILE *localVersionFile = fopen("/RetroRewind6/version.txt", "wb");
    if (!localVersionFile) {
        printf("Error opening version.txt for writing.\n");
        return;
    }
    fprintf(localVersionFile, "%s", newVersion);
    fclose(localVersionFile);
}

// Reads version info and downloads necessary files as per version file
void downloadFilesFromVersionFile(const char *versionFileURL, const char *localVersion) {
    // Download version file list
    {
        CURL *curl = nullptr;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (curl) {
            FILE *versionFile = fopen("sd:/RetroRewind6/filelist.txt", "wb");
            if (!versionFile) {
                printf("Error opening filelist.txt for writing.\n");
                curl_easy_cleanup(curl);
                curl_global_cleanup();
                return;
            }

            curl_easy_setopt(curl, CURLOPT_URL, versionFileURL);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, versionFile);
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK)
                fprintf(stderr, "Failed to download file list: %s\n", curl_easy_strerror(res));
            fclose(versionFile);
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
    }

    FILE *versionFileContents = fopen("sd:/RetroRewind6/filelist.txt", "r");
    if (!versionFileContents) {
        printf("Error opening filelist.txt for reading.\n");
        return;
    }

    filesDownloaded = 0;
    filesToDownload = 0;

    char onlineVersion[MAX_VERSION_LENGTH];
    char url[MAX_URL_LENGTH];
    char outputFilename[MAX_FILENAME_LENGTH];
    char directoryName[MAX_FIELD_LENGTH];

    // First pass to count files to download
    while (fscanf(versionFileContents, "%s %s %s %s", onlineVersion, url, outputFilename, directoryName) == 4) {
        if (compareVersions(localVersion, onlineVersion) < 0) {
            filesToDownload++;
        }
    }
    rewind(versionFileContents);

    // Second pass: download files
    while (fscanf(versionFileContents, "%s %s %s %s", onlineVersion, url, outputFilename, directoryName) == 4) {
        if (compareVersions(localVersion, onlineVersion) < 0) {
            downloadFile(url, outputFilename);
            updateLocalVersion(onlineVersion);
        } else if (compareVersions(localVersion, onlineVersion) == 0) {
            printf("Local version %s is up-to-date.\n", localVersion);
        } else {
            printf("Skipping outdated version %s.\n", onlineVersion);
        }
    }
    fclose(versionFileContents);
}

// Deletes all files in the given directory
void deleteFilesInDirectory(const char *directoryPath) {
    DIR *dir = opendir(directoryPath);
    if (!dir) {
        perror("Error opening directory");
        return;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, "..")) {
            char filePath[MAX_PATH_LENGTH];
            snprintf(filePath, sizeof(filePath), "%s/%s", directoryPath, ent->d_name);
            remove(filePath);
        }
    }
    closedir(dir);
    printf("All files in %s have been deleted.\n", directoryPath);
}

// Handles deletion of files as specified by the remote delete file
void DeleteFilesFromVersionFile(const char *deleteFileURL, const char *localVersion) {
    // Download delete file
    {
        CURL *curl = nullptr;
        curl_global_init(CURL_GLOBAL_DEFAULT);
        curl = curl_easy_init();
        if (curl) {
            FILE *deleteFile = fopen("/RetroRewind6/delete.txt", "wb");
            if (!deleteFile) {
                printf("Error opening delete.txt for writing.\n");
                curl_easy_cleanup(curl);
                curl_global_cleanup();
                return;
            }
            curl_easy_setopt(curl, CURLOPT_URL, deleteFileURL);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, deleteFile);
            CURLcode res = curl_easy_perform(curl);
            if (res != CURLE_OK)
                fprintf(stderr, "Failed to download delete file: %s\n", curl_easy_strerror(res));
            fclose(deleteFile);
            curl_easy_cleanup(curl);
        }
        curl_global_cleanup();
    }

    FILE *deleteFileContents = fopen("/RetroRewind6/delete.txt", "r");
    if (!deleteFileContents) return;

    char onlineVersion[MAX_VERSION_LENGTH];
    char fileToDelete[MAX_DIRECTORY_LENGTH];
    while (fscanf(deleteFileContents, "%s %s", onlineVersion, fileToDelete) == 2) {
        if (compareVersions(localVersion, onlineVersion) < 0) {
            // Delete specified file or directory
            if (fs::exists(fileToDelete)) {
                if (fs::is_directory(fileToDelete)) {
                    deleteFilesInDirectory(fileToDelete);
                } else {
                    remove(fileToDelete);
                }
            }
        }
    }
    fclose(deleteFileContents);
    remove("sd:/RetroRewind6/delete.txt");
}

// Main update confirmation and process routine
void UpdateIsConfirmed() {
    Haxx_UnMount(&Mounted);
    initfat = fatInitDefault();
    if (!initfat) {
        printf("Make sure your SD card is inserted!\n");
        ProgressText->SetText("Update Failed!");
        return;
    }

    curl_global_init(CURL_GLOBAL_ALL);
    printf("libcurl version: %s\n", curl_version());

    // Initialize wiisocket with retries
    int socket_init_success = -1;
    for (int attempts = 0; attempts < 3; attempts++) {
        socket_init_success = wiisocket_init();
        printf("attempt: %d wiisocket_init: %d\n", attempts, socket_init_success);
        if (socket_init_success == 0) {
            ProgressText->SetText("Starting.");
            break;
        }
    }
    if (socket_init_success != 0) {
        printf("failed to init wiisocket\n");
        ProgressText->SetText("Update Failed!");
        return;
    }

    // Attempt to get an IP with retries
    u32 ip = 0;
    for (int attempts = 0; attempts < 3; attempts++) {
        ip = gethostid();
        printf("attempt: %d gethostid: %x\n", attempts, ip);
        if (ip) {
            ProgressText->SetText("Starting.");
            break;
        }
    }
    if (!ip) {
        printf("failed to get IP\n");
        ProgressText->SetText("Update Failed!");
        return;
    }

    const char *deleteFileURL = "http://update.zplwii.xyz:8000/RetroRewind/RetroRewindDelete.txt";
    const char *fileListURL = "http://update.zplwii.xyz:8000/RetroRewind/RetroRewindVersion.txt";

    FILE *localVersionFile = fopen("/RetroRewind6/version.txt", "r");
    if (!localVersionFile) {
        printf("Error opening version.txt for reading.\n");
        ProgressText->SetText("Update Failed!");
        return;
    }

    char localVersion[MAX_VERSION_LENGTH] = {0};
    fscanf(localVersionFile, "%15s", localVersion);
    fclose(localVersionFile);

    ProgressText->SetText("Preparing...");
    DeleteFilesFromVersionFile(deleteFileURL, localVersion);
    ProgressText->SetText("Downloading...");
    downloadFilesFromVersionFile(fileListURL, localVersion);
    
    // Remove filelist.txt after downloads are complete
    if (fs::exists("sd:/RetroRewind6/filelist.txt")) {
        if (remove("sd:/RetroRewind6/filelist.txt") != 0) {
            perror("Error deleting filelist.txt");
        } else {
            printf("filelist.txt successfully deleted.\n");
        }
    }

    ProgressText->SetText("Finished!");
}
