# modem/: SIM fix

| File | What it is |
|---|---|
| `sim-wait.conf` | A systemd drop-in that makes Nura's `msm-modem-uim-selection` service wait up to 30 s for the SIM card instead of 4 s |

**Why:** at boot, `msm-modem-uim-selection` selects the SIM application on the card
so that ModemManager can use it. The Mi Max 2 modem reads the card later than 4 s,
so nothing was selected and ModemManager reported the SIM as missing: no calls, SMS
or mobile data. With the longer wait all three work. This file only changes the
existing Nura service; it adds no program.

`install.sh --sim` puts it in
`/etc/systemd/system/msm-modem-uim-selection.service.d/`. It works on any Nura version.
The full diagnosis is in "SIM card and modem" in the main README.
