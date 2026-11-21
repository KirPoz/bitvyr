# SDK bootstrap

Это project-local Espressif Hosted recipe/Kconfig с сохранёнными upstream
лицензиями. Main CMake регистрирует только bootstrap, чтобы Root мог создать SDK
dependencies и generated linker/config files. `bootstrap.c` не входит в конечный
C6 audio archive. Его ELF не предназначен для платы.

Настоящие39 TU, включая C6ProbeMain app_main, выбирает `../tools/build.py` по
`../recipe/compile41.json`;20 Hosted/собственных baseline TU также пересобираются
из исходников. Upstream/generated C и protobuf не переводятся и не получают
фиктивных собственных контрактов. Сохраняемые C6Workload/C6ProbeMain — собственные
C++17 entrypoints; нельзя считать sdkconfig или build success аппаратным join.
