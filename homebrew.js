async function main() {
  const PAYLOAD = window.workingDir + '/dump_installer.elf';

  return {
    mainText: 'Dump Installer',
    secondaryText: 'Install a dumped game',
    onclick: async () => {
      const CWD = await pickDirectory('', 'Select Game Directory...');
      if (!CWD) return;

      let args = [];

      /* ---------- Try param.json ---------- */
      try {
        const PARAM_JSON_URL = baseURL + '/fs/' + CWD + '/sce_sys/param.json';
        const resp = await fetch(PARAM_JSON_URL);

        if (resp.ok) {
          const param = await resp.json();
          if (param.titleId) {
            args = [PAYLOAD, param.titleId];
            return {
              path: PAYLOAD,
              cwd: CWD,
              args,
              daemon: true
            };
          }
        }
      } catch (e) {}

      /* ---------- Fallback to param.sfo ---------- */
      try {
        const PARAM_SFO_URL = baseURL + '/fs/' + CWD + '/sce_sys/param.sfo';
        const resp = await fetch(PARAM_SFO_URL);

        if (resp.ok) {
          // assuming your backend exposes param.sfo as JSON
          const sfo = await resp.json();

          if (sfo.TITLE_ID) {
            args = [PAYLOAD, sfo.TITLE_ID];
          }
        }
      } catch (e) {}

      return {
        path: PAYLOAD,
        cwd: CWD,
        args,
        daemon: true
      };
    }
  };
}
