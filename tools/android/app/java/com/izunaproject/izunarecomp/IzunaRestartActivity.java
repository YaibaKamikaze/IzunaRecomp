// [IzunaRecomp] Starts the app again for the hub (IzunaActivity.restartApp):
// it runs in its own process (":restart", AndroidManifest.xml), so it outlives
// the game's process, which it ends; then it opens IzunaActivity afresh and
// goes away itself.
package com.izunaproject.izunarecomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

public class IzunaRestartActivity extends Activity {
    static final String EXTRA_PID = "izuna_pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(EXTRA_PID, -1);
        if (pid > 0) android.os.Process.killProcess(pid);
        Intent again = new Intent(this, IzunaActivity.class);
        again.putExtra(IzunaActivity.EXTRA_ARGS, getIntent().getStringExtra(IzunaActivity.EXTRA_ARGS));
        again.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(again);
        finish();
        Runtime.getRuntime().exit(0);
    }
}
