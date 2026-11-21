/* Same official ESP32-P4 SDK SPM region as the existing PPU experiment. */
ASSERT(_ZN3nes4core7Cpu650213runQuietBlockEj >= ADDR(.spm.text) &&
       _ZN3nes4core7Cpu650213runQuietBlockEj < ADDR(.spm.text) + SIZEOF(.spm.text),
       "NES Cpu6502::runQuietBlock was not placed in SPM")
ASSERT(_spm_text_start == 0x30100000 && _spm_data_end <= 0x30102000,
       "NES CPU quiet SPM placement exceeds the ESP32-P4 8KiB region")
