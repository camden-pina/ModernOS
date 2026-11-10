// Electron's default app loader keeps the app directory in argv. The bundled
// workbench uses packaged-app argument parsing, so remove that loader operand
// before its entry point reads the requested project and application options.
import * as coastPortFS from 'node:fs';
// Use the workbench's supported in-window dialogs on the native single-surface
// port. Seed a fresh profile only; saved user choices remain authoritative.
const coastPortUser = '/home/port-test/.config/CodeOSS/User';
coastPortFS.mkdirSync(coastPortUser, { recursive: true, mode: 0o700 });
try {
    coastPortFS.writeFileSync(coastPortUser + '/settings.json', JSON.stringify({
        'window.dialogStyle': 'custom',
        'files.simpleDialog.enable': true,
        'window.titleBarStyle': 'custom',
        'window.controlsStyle': 'hidden',
        'window.menuStyle': 'custom',
        'workbench.reduceMotion': 'on',
        // Rounded floating panes create full-size offscreen passes in the
        // software compositor even for a single changed character. Use the
        // supported classic pane layout; retain the native desktop frame.
        'workbench.experimental.modernUI': false,
        'workbench.startupEditor': 'none',
        'editor.cursorBlinking': 'solid',
        'editor.cursorSmoothCaretAnimation': 'off',
        'editor.smoothScrolling': false,
        'workbench.list.smoothScrolling': false,
        'editor.minimap.enabled': false,
        // One semantic server provides the complete language service while
        // avoiding a second Electron image in this bounded two-CPU port.
        'js/ts.tsserver.useSyntaxServer': 'never'
    }, null, 2) + '\n', { flag: 'wx', mode: 0o600 });
} catch (error) {
    if (error.code !== 'EEXIST') throw error;
}
if (process.defaultApp) {
    const appOperand = process.argv.indexOf('/usr/share/codeoss-workbench', 1);
    if (appOperand !== -1) process.argv.splice(appOperand, 1);
}
