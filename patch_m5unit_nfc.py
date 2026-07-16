Import("env")

from pathlib import Path


def patch_file(path: Path, old: str, new: str) -> None:
    if not path.exists():
        return
    text = path.read_text()
    if new in text:
        return
    if old not in text:
        print("[M5UnitNFC] patch pattern not found: {}".format(path))
        return
    path.write_text(text.replace(old, new, 1))
    print("[M5UnitNFC] patched {}".format(path))


root = Path(env.subst("$PROJECT_DIR"))
for libroot in (root / ".pio" / "libdeps").glob("*/M5UnitNFC"):
    layer = libroot / "src" / "nfc" / "layer" / "a" / "emulation_layer_a.cpp"
    patch_file(
        layer,
        "if (!(picc.isNTAG2() || picc.type == Type::MIFARE_Ultralight)) {",
        "if (!(picc.isNTAG2() || picc.type == Type::MIFARE_Ultralight ||\n"
        "          picc.type == Type::MIFARE_Classic_1K || picc.type == Type::MIFARE_Classic_4K)) {",
    )

    # Keep the official NFC-A lifecycle: begin() enters Off and waits for the
    # reader field. MFC uses its own proven RF safety net in GroveNFC.cpp.
    listener = libroot / "src" / "nfc" / "layer" / "a" / "emulation_layer_a_ST25R3916.cpp"
    patch_file(
        listener,
        "    // Enter listen state immediately. Waiting in Off for I_eon32 loses\n"
        "    // the field-detect IRQ on some I2C Unit firmware revisions.\n"
        "    (void)goto_idle();\n    return true;",
        "    (void)goto_off();\n    return true;",
    )
    patch_file(
        listener,
        "    } else {\n"
        "        if (_layer.emulatePICC().type == Type::MIFARE_Classic_1K ||\n"
        "            _layer.emulatePICC().type == Type::MIFARE_Classic_4K) {\n"
        "            return goto_idle();\n"
        "        }\n"
        "        _u.clear_bit_register8(REG_OPERATION_CONTROL, tx_en | rx_en | en);\n"
        "    }\n"
        "    return EmulationLayerA::State::Off;",
        "    } else {\n"
        "        _u.clear_bit_register8(REG_OPERATION_CONTROL, tx_en | rx_en | en);\n"
        "    }\n"
        "    return EmulationLayerA::State::Off;",
    )
    patch_file(
        listener,
        "    } else {\n        return goto_idle();\n    }\n    return EmulationLayerA::State::Off;",
        "    } else {\n        _u.clear_bit_register8(REG_OPERATION_CONTROL, tx_en | rx_en | en);\n    }\n    return EmulationLayerA::State::Off;",
    )
    patch_file(
        listener,
        "    if ((get_irq(I_eon32) & I_eon32)) {\n"
        "        return goto_idle();\n"
        "    }",
        "    if ((get_irq(I_eon32) & I_eon32) || is_extra_field()) {\n"
        "        return goto_idle();\n"
        "    }",
    )
