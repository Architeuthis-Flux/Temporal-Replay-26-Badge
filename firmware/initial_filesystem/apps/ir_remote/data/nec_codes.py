"""Curated NEC consumer-IR codebook.

Format: VENDORS = (vendor_name, ((category, ((label, addr, cmd), ...)), ...))

These are well-known NEC codes that survive on TVs / amplifiers / set-top
boxes shipped from ~2005 onward. Sony / RC5 / RC6 vendors live in raw-mode
captures; they are deliberately not represented here.
"""

VENDORS = (
    (
        "Samsung",
        (
            (
                "Power",
                (
                    ("Power", 0x07, 0x02),
                    ("Standby", 0x07, 0x40),
                ),
            ),
            (
                "Volume",
                (
                    ("Vol+", 0x07, 0x07),
                    ("Vol-", 0x07, 0x0B),
                    ("Mute", 0x07, 0x0F),
                ),
            ),
            (
                "Channels",
                (
                    ("Ch+", 0x07, 0x12),
                    ("Ch-", 0x07, 0x10),
                ),
            ),
        ),
    ),
    (
        "LG",
        (
            (
                "Power",
                (
                    ("Power", 0x04, 0x08),
                ),
            ),
            (
                "Volume",
                (
                    ("Vol+", 0x04, 0x02),
                    ("Vol-", 0x04, 0x03),
                    ("Mute", 0x04, 0x09),
                ),
            ),
            (
                "Channels",
                (
                    ("Ch+", 0x04, 0x00),
                    ("Ch-", 0x04, 0x01),
                ),
            ),
        ),
    ),
    (
        "NEC",
        (
            (
                "Power",
                (
                    ("Power", 0x00, 0x12),
                ),
            ),
            (
                "Volume",
                (
                    ("Vol+", 0x00, 0x10),
                    ("Vol-", 0x00, 0x11),
                ),
            ),
        ),
    ),
    (
        "Vizio",
        (
            (
                "Power",
                (
                    ("Power", 0x14, 0x08),
                ),
            ),
            (
                "Volume",
                (
                    ("Vol+", 0x14, 0x02),
                    ("Vol-", 0x14, 0x03),
                    ("Mute", 0x14, 0x09),
                ),
            ),
        ),
    ),
    (
        "Pioneer",
        (
            (
                "Power",
                (
                    ("Power", 0xA5, 0x38),
                ),
            ),
            (
                "Volume",
                (
                    ("Vol+", 0xA5, 0x0A),
                    ("Vol-", 0xA5, 0x0B),
                ),
            ),
        ),
    ),
    (
        "Onkyo",
        (
            (
                "Power",
                (
                    ("Power", 0xD2, 0x12),
                ),
            ),
            (
                "Volume",
                (
                    ("Vol+", 0xD2, 0x40),
                    ("Vol-", 0xD2, 0x41),
                ),
            ),
        ),
    ),
)
