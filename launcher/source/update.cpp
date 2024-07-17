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

RiiDisc Disc2;
vector < int > Mounted2;
static void * xfb = NULL;
static GXRModeObj * rmode = NULL;

#define MAX_URL_LENGTH 256
#define MAX_FILENAME_LENGTH 128
#define MAX_DIRECTORY_LENGTH 128
#define MAX_VERSION_LENGTH 16
#define MAX_PATH_LENGTH 256
#define MAX_FIELD_LENGTH 112
#define MAX_PATH 256

// Global variables to keep track of total size and downloaded size
static double totalSize = 0;
static double downloadedSize = 0;

char filePath[MAX_PATH];

bool done2 = false;
bool die2 = false;
int filesToDownload = 0;
int filesDownloaded = 0;
int totalFiles = 0; // Declare totalFiles variable

void timetostop() {
  die2 = done2;
}
static bool initfat = false;

struct memory {
  char * response;
  size_t size;
};

size_t WriteCallback(void * contents, size_t size, size_t nmemb, void * userp) {
  size_t realsize = size * nmemb;
  FILE * fp = (FILE * ) userp;
  return fwrite(contents, size, nmemb, fp);
}

int compareVersions(const char * localVersion,
  const char * onlineVersion) {
  // Split version strings into components
  int localMajor, localMinor, localPatch;
  int onlineMajor, onlineMinor, onlinePatch;

  sscanf(localVersion, "%d.%d.%d", & localMajor, & localMinor, & localPatch);
  sscanf(onlineVersion, "%d.%d.%d", & onlineMajor, & onlineMinor, & onlinePatch);

  // Compare major version
  if (localMajor < onlineMajor) {
    return -1;
  } else if (localMajor > onlineMajor) {
    return 1;
  }

  // Compare minor version
  if (localMinor < onlineMinor) {
    return -1;
  } else if (localMinor > onlineMinor) {
    return 1;
  }

  // Compare patch version
  if (localPatch < onlinePatch) {
    return -1;
  } else if (localPatch > onlinePatch) {
    return 1;
  }

  // Versions are equal
  return 0;
}

int ProgressCallback(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    // Calculate progress for the current file
    double fileProgress = (dlnow / dltotal) * 100.0;

    // Calculate overall progress percentage including completed files
    double overallProgress = (((double)filesDownloaded / filesToDownload) * 100.0) + (fileProgress / filesToDownload);

    // Ensure it ends at 100 percent
    if (dlnow == dltotal && filesDownloaded == filesToDownload - 1) {
        overallProgress = 100.0;
    }

    // Update progress text
    char progressText[40];
    snprintf(progressText, sizeof(progressText), "%.0f%%", overallProgress);
    ProgressText->SetText(progressText);

    return 0;
}

void extractZip(const char *zipFilename, const char *outputDirectory) {
    unzFile zipFile = unzOpen(zipFilename);
    if (zipFile == NULL) {
        printf("Error opening zip file %s\n", zipFilename);
        return;
    }

    if (unzGoToFirstFile(zipFile) != UNZ_OK) {
        printf("Failed to go to first file in zip\n");
        unzClose(zipFile);
        return;
    }

    char extractedPath[MAX_PATH_LENGTH];
    getcwd(extractedPath, sizeof(extractedPath));
    chdir(outputDirectory);

    // Go through each file in the zip and extract it
    int status;
    do {
        char filename[MAX_FILENAME_LENGTH];
        unz_file_info file_info;
        if (unzGetCurrentFileInfo(zipFile, &file_info, filename, sizeof(filename), NULL, 0, NULL, 0) != UNZ_OK) {
            printf("Failed to get file info for file in zip\n");
            continue;
        }

        status = unzOpenCurrentFile(zipFile);
        if (status != UNZ_OK) {
            printf("Failed to open file in zip: %d\n", status);
            continue;
        }

        // Create directories if they don't exist
        char directoryPath[MAX_PATH_LENGTH];
        strncpy(directoryPath, filename, strrchr(filename, '/') - filename);
        directoryPath[strrchr(filename, '/') - filename] = '\0';

        if (!fs::exists(directoryPath)) {
            fs::create_directories(directoryPath);
        }

        FILE *fp = fopen(filename, "wb");
        if (fp == NULL) {
            printf("Failed to open file %s for writing\n", filename);
            unzCloseCurrentFile(zipFile);
            continue;
        }

        int read;
        char buffer[4096];
        while ((read = unzReadCurrentFile(zipFile, buffer, sizeof(buffer))) > 0) {
            fwrite(buffer, 1, read, fp);
        }

        fclose(fp);
        unzCloseCurrentFile(zipFile);
        printf("Extracted: %s\n", filename);
    } while (unzGoToNextFile(zipFile) == UNZ_OK);

    unzClose(zipFile);
    chdir(extractedPath);
}


void downloadFile(const char *url, const char *outputFilename) {
    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        // Create directories if they don't exist
        char directoryPath[MAX_PATH_LENGTH];
        strncpy(directoryPath, outputFilename, strrchr(outputFilename, '/') - outputFilename);
        directoryPath[strrchr(outputFilename, '/') - outputFilename] = '\0';

        // Check if directory exists, if not create it
        if (!fs::exists(directoryPath)) {
            fs::create_directories(directoryPath);
        }

        FILE *fp = fopen(outputFilename, "wb");
        if (fp == NULL) {
            printf("Error opening file %s for writing.\n", outputFilename);
            exit(0);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, ProgressCallback);

        // Reset progress for new file
        totalSize = 0;
        downloadedSize = 0;

        res = curl_easy_perform(curl);

        fclose(fp);

        if (res != CURLE_OK) {
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));
        } else {
            printf("Downloaded: %s\n", outputFilename);

            // Check if it's a zip file
            if (strstr(outputFilename, ".zip") != NULL) {
                // Extract the zip file to a temporary directory
                extractZip(outputFilename, "temp_extract");

                // Remove the downloaded zip file
                remove(outputFilename);
            }

            // Increment filesDownloaded count after a successful download
            filesDownloaded++;
        }

        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();
}

void updateLocalVersion(const char * newVersion) {
  FILE * localVersionFile = fopen("/RetroRewind6/version.txt", "wb");
  if (localVersionFile == NULL) {
    printf("Error opening version.txt for writing.\n");
    exit(0);
  }

  fprintf(localVersionFile, "%s", newVersion);
  fclose(localVersionFile);
}

void downloadFilesFromVersionFile(const char *versionFileURL, const char *localVersion) {
    CURL *curl;
    CURLcode res;

    curl_global_init(CURL_GLOBAL_DEFAULT);
    curl = curl_easy_init();

    if (curl) {
        FILE *versionFile = fopen("sd:/RetroRewind6/filelist.txt", "wb");
        if (versionFile == NULL) {
            printf("Error opening version.txt for writing.\n");
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, versionFileURL);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, versionFile);

        res = curl_easy_perform(curl);

        if (res != CURLE_OK)
            fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

        fclose(versionFile);
        curl_easy_cleanup(curl);
    }

    curl_global_cleanup();

    printf("Contents of version.txt:\n");
    FILE *versionFileContents = fopen("sd:/RetroRewind6/filelist.txt", "r");
    if (versionFileContents) {
        // Reset filesDownloaded count
        filesDownloaded = 0;
        filesToDownload = 0;

        char onlineVersion[MAX_VERSION_LENGTH];
        char url[MAX_URL_LENGTH];
        char outputFilename[MAX_FILENAME_LENGTH];
        char directoryName[MAX_FIELD_LENGTH];

        // First pass: Count the number of files to download
        while (fscanf(versionFileContents, "%s %s %s %s", onlineVersion, url, outputFilename, directoryName) == 4) {
            int comparisonResult = compareVersions(localVersion, onlineVersion);
            if (comparisonResult < 0) {
                filesToDownload++;
            }
        }
        rewind(versionFileContents);

        // Second pass: Download the files
        while (fscanf(versionFileContents, "%s %s %s %s", onlineVersion, url, outputFilename, directoryName) == 4) {
            // Construct the full path including the directory
            char fullPath[MAX_FILENAME_LENGTH];
            snprintf(fullPath, sizeof(fullPath), "sd:/RetroRewind6/%s", directoryName);

            // Extract directory path from the filePath
            char directory[MAX_FILENAME_LENGTH];
            strncpy(directory, fullPath, strrchr(fullPath, '/') - fullPath);
            directory[strrchr(fullPath, '/') - fullPath] = '\0';

            mkdir(fullPath, 0777);

            int comparisonResult = compareVersions(localVersion, onlineVersion);

            if (comparisonResult < 0) {
                downloadFile(url, outputFilename);
                printf("Downloaded: %s\n", outputFilename);
                updateLocalVersion(onlineVersion);
            } else if (comparisonResult == 0) {
                printf("Local version is up-to-date.\n");
            } else {
                printf("Skipping download for version %s (already up-to-date).\n", onlineVersion);
            }
        }

        // Close version file
        fclose(versionFileContents);
    }
}

void deleteFilesInDirectory(const char * directoryPath) {
  DIR * dir;
  struct dirent * ent;

  // Open the directory
  if ((dir = opendir(directoryPath)) != NULL) {
    // Iterate through each entry in the directory
    while ((ent = readdir(dir)) != NULL) {
      if (strcmp(ent -> d_name, ".") != 0 && strcmp(ent -> d_name, "..") != 0) {
        // Create the full path of the file
        char filePath[MAX_PATH];
        snprintf(filePath, sizeof(filePath), "%s/%s", directoryPath, ent -> d_name);

        // Remove the file
        remove(filePath);
      }
    }

    // Close the directory
    closedir(dir);

    printf("All files in the directory have been deleted.\n");
  } else {
    // Handle error opening directory
    perror("Error opening directory");
    return;
  }
}

void DeleteFilesFromVersionFile(const char * DeleteFileURL,
  const char * localVersion) {
  CURL * curl;
  CURLcode res;

  curl_global_init(CURL_GLOBAL_DEFAULT);
  curl = curl_easy_init();

  if (curl) {
    FILE * DeleteFile = fopen("/RetroRewind6/delete.txt", "wb");
    if (DeleteFile == NULL) {
      printf("Error opening version.txt for writing.\n");
      return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, DeleteFileURL);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, DeleteFile);

    res = curl_easy_perform(curl);

    if (res != CURLE_OK)
      fprintf(stderr, "curl_easy_perform() failed: %s\n", curl_easy_strerror(res));

    fclose(DeleteFile);
    curl_easy_cleanup(curl);
  }

  FILE * DeleteFileContents = fopen("/RetroRewind6/delete.txt", "r");
  if (DeleteFileContents) {
    char onlineVersion[MAX_VERSION_LENGTH];
    char FileToDelete[MAX_DIRECTORY_LENGTH];

    while (fscanf(DeleteFileContents, "%s %s", onlineVersion, FileToDelete) == 2) {
      int comparisonResult = compareVersions(localVersion, onlineVersion);

      if (comparisonResult < 0) {
        // Delete files in the directory
        unlink(localVersion);
      }
    }
    fclose(DeleteFileContents);
    unlink("sd:/RetroRewind6/delete.txt");
  }
}

void UpdateIsConfirmed() {
  Haxx_UnMount( & Mounted2);
  initfat = fatInitDefault();
  if (!initfat) {
    // SD card not present or error initializing
    printf("Make sure your SD card is inserted!\n");
    ProgressText -> SetText("Update Failed!"); // Set progress text to indicate failure
    return;
  } {
    curl_global_init(CURL_GLOBAL_ALL);

    printf("libcurl version: %s\n", curl_version());

    // try a few times to initialize libwiisocket (?)
    int socket_init_success = -1;
    for (int attempts = 0; attempts < 3; attempts++) {
      socket_init_success = wiisocket_init();
      printf("attempt: %d wiisocket_init: %d\n", attempts, socket_init_success);
      if (socket_init_success == 0) {
        ProgressText -> SetText("Starting.");
        break;
      }
    }
    if (socket_init_success != 0) {
      puts("failed to init wiisocket");
      ProgressText -> SetText("Update Failed!"); // Set progress text to indicate failure
      return;
    }

    // try a few times to get an IP (?)
    u32 ip = 0;
    for (int attempts = 0; attempts < 3; attempts++) {
      ip = gethostid();
      printf("attempt: %d gethostid: %x\n", attempts, ip);
      if (ip) {
        ProgressText -> SetText("Starting.");
        break;
      }
    }
    if (!ip) {
      puts("failed to get IP");
      ProgressText -> SetText("Update Failed!"); // Set progress text to indicate failure
      return;
    }

    const char * fileToDelete = "http://104.198.244.247:8000/RetroRewind/RetroRewindDelete.txt";
    const char * fileListURL = "http://104.198.244.247:8000/RetroRewind/RetroRewindVersion.txt";

    // Read local version from a local file
    FILE * localVersionFile = fopen("/RetroRewind6/version.txt", "r");
    if (localVersionFile == NULL) {
      printf("Error opening version.txt for reading.\n");
      ProgressText -> SetText("Update Failed!"); // Set progress text to indicate failure
      return;
    }

    ProgressText -> SetText("Starting."); // Set initial progress text

    char localVersion[MAX_VERSION_LENGTH];
    fscanf(localVersionFile, "%s", localVersion);
    fclose(localVersionFile);

    ProgressText -> SetText("Preparing...");

    // Delete files
    DeleteFilesFromVersionFile(fileToDelete, localVersion);

    ProgressText -> SetText("Downloading..."); // Set progress text to indicate downloading

    // Download version.txt (which also contains filelist)
    downloadFilesFromVersionFile(fileListURL, localVersion);

    ProgressText -> SetText("Finished!"); // Set progress text to indicate finished
  }
  return;
}