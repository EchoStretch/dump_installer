async function main() {
  const PAYLOAD = window.workingDir + '/dump_installer.elf';

  return {
    mainText: 'Dump Installer',
    secondaryText: 'By EchoStretch',
    onclick: async () => {
      const CWD = await pickDirectory('', 'Select Game Directory...');
      return CWD ? {
        path: PAYLOAD,
        cwd: CWD,
        daemon: true
      } : {};
    }
  };
}