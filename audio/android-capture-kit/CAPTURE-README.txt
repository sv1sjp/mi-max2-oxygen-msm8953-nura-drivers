SPEAKER DSP CAPTURE — quick instructions
========================================

WHAT THIS IS
  A small script that records what an Android phone's audio system tells its
  DSP chip when the LOUDSPEAKER plays. This helps fix the loudspeaker on
  postmarketOS (Linux) for the same chip. You don't need to understand it.

BEST PHONE TO RUN IT ON (in order)
  1. Xiaomi Mi Max 2 ("oxygen") running Android (e.g. Pixel Experience A13) —
     exact same hardware, most useful.
  2. Any other Xiaomi/Android phone with a Snapdragon 625/626 (msm8953) chip
     whose loudspeaker works on Android. (Redmi Note 4, Mi A2 Lite, Redmi 5
     Plus, Moto G5S, Asus Zenfone 4, etc.)
  The phone MUST be ROOTED (Magisk or similar). Without root it can't run.

WHAT YOU NEED
  - The phone, rooted, with its loudspeaker working on Android.
  - Either a PC with "adb", OR the Termux app on the phone with root.

HOW TO RUN — option A (PC with adb)
  1. adb push capture-speaker-dsp.sh /data/local/tmp/
  2. adb shell su -c 'sh /data/local/tmp/capture-speaker-dsp.sh'
  3. When it says "play sound on the loudspeaker NOW", start a song or ringtone
     on the SPEAKER (not headphones, not the earpiece) and let it play ~10s.
  4. When it prints PASS/FAIL and a file path, pull the file:
        adb pull /sdcard/speaker-dsp-capture.txt
  5. Send that one file back.

HOW TO RUN — option B (Termux app, no PC)
  1. Put capture-speaker-dsp.sh in the phone's internal storage (/sdcard).
  2. Open Termux, type:  su         (grant root when asked)
  3. Type:  sh /sdcard/capture-speaker-dsp.sh
  4. When it says to play sound, play a song/ringtone ON THE SPEAKER ~10s.
  5. It saves /sdcard/speaker-dsp-capture.txt — send that one file back.

IMPORTANT
  - The sound must come out of the LOUDSPEAKER during the 8-second capture.
    If headphones are plugged in, unplug them first.
  - At the end it prints PASS, PARTIAL, or FAIL. If PASS, great. If not, just
    re-run it and make sure the speaker is actually playing. Either way, the
    saved file is still worth sending.
  - The file contains only audio-routing/debug data and basic device info
    (model, kernel). No personal data, no audio recording of the sound itself.

WHAT TO SEND BACK
  The single file:  speaker-dsp-capture.txt
