async function scanApps() {
  const APP_AUTO_SCAN_PATHS = ["/data/psapp"];

  const APP_AUTO_SCAN_PATH_LISTFILES = ["/data/psapp.lst",
          "/mnt/usb0/psapp.lst", "/mnt/usb1/psapp.lst",
          "/mnt/usb2/psapp.lst", "/mnt/usb3/psapp.lst",
          "/mnt/usb4/psapp.lst", "/mnt/usb5/psapp.lst",
          "/mnt/usb6/psapp.lst", "/mnt/usb7/psapp.lst",
          "/mnt/ext0/psapp.lst", "/mnt/ext1/psapp.lst"];

  const APP_AUTO_SCAN_EXEC_NAMES = [ "eboot.bin" ];

  let apps = [];
  let jobs = [];
  for (let file of APP_AUTO_SCAN_PATH_LISTFILES) {
    let filePath = file.substring(0, file.lastIndexOf("/") + 1);
    let dirList = await ApiClient.fsGetFileText(file);
    if (dirList.data !== null) {
      for (let dir of dirList.data.split(/\r?\n/)) {
        dir = dir.trim();
        if (dir !== "") {
          if (!dir.startsWith("/")) {
            dir = filePath + dir;
          }

          if (!APP_AUTO_SCAN_PATHS.includes(dir)) {
            APP_AUTO_SCAN_PATHS.push(dir);
          }
        }
      }
    }
  }

  for (let path of APP_AUTO_SCAN_PATHS) {
    jobs.push((async () => {
      let autoScanParentDirEntryList = await ApiClient.fsListDir(path);
      if (autoScanParentDirEntryList.data === null) {
        return;
      }
      if (!path.endsWith("/")) {
        path += "/";
      }

      let subjobs = [];

      for (let entry of autoScanParentDirEntryList.data) {
        subjobs.push((async () => {
          let appDirEntries = await ApiClient.fsListDir(path + entry.name);
          if (appDirEntries.data === null) {
            return;
          }

          let foundExecutableName = null;
          for (let hbDirEntry of appDirEntries.data) {
            for (let execName of APP_AUTO_SCAN_EXEC_NAMES) {
              if (hbDirEntry.name === execName) {
                foundExecutableName = execName;
                break;
              }
            }

            if (foundExecutableName) {
              break;
            }
          }

          if (foundExecutableName) {
            apps.push(path + entry.name);
          }
        })());
      }

      await Promise.all(subjobs);
    })());
  }

  await Promise.all(jobs);
  return apps;
}

async function getAppInfo(path) {
  let storage = 'INT';
  let appId = path.substring(path.lastIndexOf("/") + 1).split('-')[0];
  let title = appId;

  if (path.startsWith('/mnt/usb')) {
    storage = 'USB';
  } else if (path.startsWith('/mnt/ext')) {
    storage = 'EXT';
  }

  try {
    const PARAM_URL = baseURL + '/fs/' + path + '/sce_sys/param.json';
    const resp = await fetch(PARAM_URL);
    const param = await resp.json();

    if (param.titleId !== undefined) {
      appId = param.titleId;
    } else if (param.title_id !== undefined) {
      appId = param.title_id;
    }

    for (const key in param.localizedParameters) {
      if (key.startsWith('en-')) {
        title = param.localizedParameters[key]['titleName'];
        break;
      }
    }
  } catch {}

  return [storage, appId, title];
}

async function launchAppById(appId) {
  const params = new URLSearchParams({
          "titleId": appId
        });

  const uri = baseURL + "/launch?" + params.toString();

  const response = await fetch(uri);
  if (!response.ok) {
    return false;
  }

  return true;
}

async function isAppInstalled(appId, path) {
  try {
    const LINK_URL = baseURL + '/fs/user/app/' + appId + '/mount.lnk';
    const resp = await fetch(LINK_URL);
    const txt = await resp.text();

    return txt === path;
  } catch {}

  return false;
}

async function main() {
  const PAYLOAD = window.workingDir + '/dump_installer.elf';
  const SHOW_ADDGAME = true;

  return {
    mainText: 'Dump Installer',
    secondaryText: 'By EchoStretch',
    onclick: async () => {
      const apps = await scanApps();
      const items = [];

      for (let app of apps) {
        const [storage, appId, title] = await getAppInfo(app);
        const installed = await isAppInstalled(appId, app);
        if (!installed) {
          items.push({
            mainText: title,
            secondaryText: storage,
            imgPath: baseURL + '/fs/' + app + '/sce_sys/icon0.png',
            onclick: async () => {
              await ApiClient.launchApp(PAYLOAD, null, null, app, true);
            }
          });
        } else {
          items.push({
            mainText: title,
            secondaryText: storage + ' - Installed',
            imgPath: baseURL + '/fs/' + app + '/sce_sys/icon0.png',
            onclick: async () => {
              await launchAppById(appId);
            },
            options: [
              {
                text: "Install",
                onclick: async () => {
                  await ApiClient.launchApp(PAYLOAD, null, null, app, true);
                }
              }
            ]
          });
        }
      }

      if (SHOW_ADDGAME || !items.length) {
        items.push(
          {
            mainText: '+',
            secondaryText: 'Add Game',
            onclick: async () => {
              const app = await pickDirectory('', 'Select Game Directory...');
              if (app) {
                await ApiClient.launchApp(PAYLOAD, null, null, app, true);
              }
            }
          }
        );
      }

      showCarousel(items);
    }
  };
}