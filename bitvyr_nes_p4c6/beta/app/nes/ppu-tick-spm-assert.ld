/* Board policy: addresses match the ESP32-P4 SDK spm_idram_seg. */
ASSERT(_ZN3nes4core3Ppu8tickDotsEj >= ADDR(.spm.text) &&
       _ZN3nes4core3Ppu8tickDotsEj < ADDR(.spm.text) + SIZEOF(.spm.text),
       "NES Ppu::tickDots was not placed in SPM")
ASSERT(_spm_text_start == 0x30100000 && _spm_data_end <= 0x30102000,
       "NES SPM placement exceeds the ESP32-P4 8KiB region")
