menu "GroveNFC Application"

config GROVENFC_APP
    bool "GroveNFC Zephyr Application"
    default y
    help
      Enable the GroveNFC multi-mode NFC application.

config GROVENFC_NFC_POLL_INTERVAL_MS
    int "NFC polling interval (ms)"
    default 200
    depends on GROVENFC_APP
    help
      How often the NFC worker thread polls for cards in reader mode.

config GROVENFC_UI_STACK_SIZE
    int "UI thread stack size (bytes)"
    default 8192
    depends on GROVENFC_APP

config GROVENFC_NFC_STACK_SIZE
    int "NFC worker thread stack size (bytes)"
    default 4096
    depends on GROVENFC_APP

config GROVENFC_PIANO_NOTES
    int "Number of piano note slots"
    default 8
    depends on GROVENFC_APP

endmenu
