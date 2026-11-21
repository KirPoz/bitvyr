# Совместимый C6 codec source

ResolvedColourCodec.cpp используется только ordered C6 recipe: source-map помещает
его в projection рядом с единственным C6 ResolvedColourCodec.h. P4 runtime этот
legacy colour codec не собирает. Wire/capabilities установленного C6 worker
сохранены; удаление server feature требует отдельного нового image/admission.
