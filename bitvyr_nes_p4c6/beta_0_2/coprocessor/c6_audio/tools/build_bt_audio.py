
"""Собирает LE Audio и Classic A2DP поверх экспериментального C6 worker."""

import argparse
import hashlib
import json
from pathlib import Path

import build

PACK = build.PROJECT / "bluetooth_audio"
PROFILES = {
    "CONFIG_BT_LE_MAX_CONNECTIONS": ("3", "6"),
    "CONFIG_BT_LE_CONTROLLER_TASK_STACK_SIZE": ("4096", "8192"),
    "CONFIG_ESP_SDIO_RX_Q_SIZE": ("20", "8"),
    "CONFIG_ESP_SDIO_TX_Q_SIZE": ("20", "12"),
}


def verify_profile(path, lock):
    """Проверяет BLE бюджет и сохранение остальных значений базового sdkconfig."""
    values = build.config_values(path)
    for name, (before, after) in PROFILES.items():
        if values.get(name) != after:
            raise RuntimeError(f"{name}: expected {after}, got {values.get(name)}")
        values[name] = before
    logical = json.dumps(values, sort_keys=True, separators=(",", ":")).encode()
    if hashlib.sha256(logical).hexdigest() != lock["effective_sdkconfig_sha256"]:
        raise RuntimeError("Unsupported change in experimental C6 sdkconfig")


def input_bytes(upstream, file, sha):
    """Сверяет закреплённый upstream-файл до включения в экспериментальную сборку."""
    path = (upstream / file).resolve()
    if not path.is_relative_to(upstream):
        raise RuntimeError(f"Upstream file leaves its root: {file}")
    data = path.read_bytes()
    if hashlib.sha256(data).hexdigest() != sha:
        raise RuntimeError(f"Upstream file SHA256 mismatch: {file}")
    return data


def once(data, needle, new):
    """Применяет замену к одному известному участку закреплённого исходника."""
    if data.count(needle) != 1:
        raise RuntimeError(f"Expected exactly one source anchor: {needle!r}")
    return data.replace(needle, new, 1)


def hosted_adapter(source):
    """Добавляет обработку H4 в локальную проекцию Hosted без изменения Raw SERIAL1."""
    for needle, new in (
        (b'#include "slave_bt.h"', b'#include "slave_bt.h"\n#include "cis_c6.h"\n#include "br_c6.h"'),
        (
            b"static int host_rcv_pkt(uint8_t *data, uint16_t len)",
            b"int slave_bt_send_to_host(const uint8_t *data, uint16_t len)",
        ),
        (
            b"static esp_vhci_host_callback_t vhci_host_cb = {",
            b"static int host_rcv_pkt(uint8_t *data, uint16_t len)\n{\n"
            b"\tif (cis_c6_c2h(data, len) || br_c6_c2h(data, len)) return 0;\n"
            b"\treturn slave_bt_send_to_host(data, len);\n}\n\n"
            b"static esp_vhci_host_callback_t vhci_host_cb = {",
        ),
        (
            b"\tpayload--;\n\tpayload_len++;\n",
            b"\tpayload--;\n\tpayload_len++;\n"
            b"\tif (br_c6_h2c(payload, payload_len) || cis_c6_h2c(payload, payload_len)) return;\n",
        ),
        (
            b"#endif // BLUETOOTH_HCI\n\tbt_enabled = true;",
            b"#endif // BLUETOOTH_HCI\n"
            b"#if BLUETOOTH_HCI\n"
            b'\tif (cis_c6_init() != 0) ESP_LOGE(TAG, "CIS initialization failed");\n'
            b'\tif (br_c6_init() != 0) ESP_LOGE(TAG, "Classic initialization failed");\n'
            b"#endif\n\tbt_enabled = true;",
        ),
        (
            b"#if BLUETOOTH_HCI\n// unregister callback functions",
            b"#if BLUETOOTH_HCI\n\tbr_c6_deinit();\n\tcis_c6_deinit();\n"
            b"// unregister callback functions",
        ),
    ):
        source = once(source, needle, new)
    return source


def compile_commands(plan, project, names):
    """Расширяет прежнюю конфигурацию компилятора C6 новыми исходниками радио."""
    commands = plan["commands"]
    original = next(row for row in commands if row["label"] == "compile-slave_bt.c.obj")
    flags = original["argv"]
    compile_options = flags[:flags.index("-o")]
    compile_options.append("-DCONFIG_ESP_DEFAULT_TASK_PRIO=22")
    compile_options.extend(f"-I{project / folder}" for folder in ("c6_firmware/main", "cis_ll", "br_ll"))
    output = Path(plan["variables"]["OUT"]) / "objects"
    original["argv"] = compile_options + ["-o", flags[flags.index("-o") + 1], "-c", str(project / "slave_bt.c")]
    index = next(i for i, row in enumerate(commands) if row["label"] == "archive39")
    archive = commands[index]
    for name in names:
        source = project / name
        member = output / (source.name + ".obj")
        commands.insert(index, dict(label=f"compile-bt-{source.name}", cwd=original["cwd"], argv=[
            *compile_options, "-o", str(member), "-c", str(source),
        ]))
        index += 1
        archive["argv"].append(str(member))


def prepare(args):
    """Создаёт новый проект C6 с проверенными входами и отдельной картой линковки."""
    manifest_path = PACK / "sources.json"
    manifest = build.read(manifest_path)
    upstream = args.upstream.resolve()
    sources = manifest["files"]
    inputs = {file: input_bytes(upstream, file, sha) for file, sha in sources.items()}
    worker = build.prepare(args)
    plan = build.read(worker)
    out = worker.parent
    variables = {key: Path(value) for key, value in plan["variables"].items()}
    pins = [build.pin(manifest_path), build.pin(Path(__file__))]
    project = out / "bt_audio_src"
    for name, data in inputs.items():
        target = project / name
        target.parent.mkdir(parents=True, exist_ok=True)
        if name.endswith("cis_c6_llcp.c"):
            data = once(data, b'#include "mbedtls/aes.h"',
                        b'#define MBEDTLS_DECLARE_PRIVATE_IDENTIFIERS\n#include "mbedtls/private/aes.h"')
        if name.endswith("verify_hooks.py"):
            data = once(data, b'"libmain.a" in xp', b'"libc6_audio.a(" in xp')
        target.write_bytes(data)
        pins.append(build.pin(target))

    cfg = out / "sdkconfig"
    with cfg.open("a", encoding="utf-8") as f:
        for name, (_, value) in PROFILES.items():
            f.write(f"\n{name}={value}\n")
    adapter = project / "slave_bt.c"
    original = build.PROJECT / "project/main/slave_bt.c"
    adapter.write_bytes(hosted_adapter(original.read_bytes().replace(b"\r\n", b"\n")))
    pins.extend([build.pin(adapter), build.pin(original)])
    compile_commands(plan, project, [name for name in inputs if name.endswith(".c")])

    library = variables["IDF"] / manifest["controller_archive"]
    hooked = out / "ble_hooked/libble_app.a"
    toolchain = variables["TOOLCHAIN"] / "bin"
    scripts = project / "c6_firmware/cmake"
    compile_cmds = plan["commands"]
    link_index = next(i for i, row in enumerate(compile_cmds) if row["label"] == "link")
    link = compile_cmds[link_index]
    compile_cmds.insert(link_index, dict(label="patch-c6-controller", cwd=link["cwd"], argv=[
        str(variables["CMAKE"]),
        *(f"-D{name}={toolchain / ('riscv32-esp-elf-' + name.lower() + '.exe')}"
          for name in ("AR", "OBJCOPY", "OBJDUMP", "NM")),
        f"-DSRC={library}", f"-DDST={hooked}", f"-DHOOKS={scripts / 'ble_hooks.txt'}",
        "-P", str(scripts / "patch_ble_lib.cmake"),
    ]))
    rsp = out / "link.rsp"
    contents = rsp.read_text("utf-8")
    if contents.count(library.as_posix()) != 2:
        raise RuntimeError("Expected two BLE archive references in the pinned recipe")
    rsp.write_text(contents.replace(library.as_posix(), hooked.as_posix())
                   + '"-Wl,--wrap=esp_panic_handler"\n', encoding="utf-8", newline="\n")
    pins.append(build.pin(rsp))
    compile_cmds.insert(link_index + 2, dict(label="verify-c6-controller-hooks", cwd=link["cwd"], argv=[
        str(variables["PYTHON"]), str(scripts / "verify_hooks.py"),
        "--elf", str(out / "network_adapter.elf"), "--map", str(out / "network_adapter.map"),
        "--hooks", str(scripts / "ble_hooks.txt"), "--orig-lib", library.as_posix(),
    ]))
    plan["source_recipe"].extend(pins)
    plan["bluetooth_audio"] = dict(upstream=manifest["commit"], sdkconfig=PROFILES,
                                 provenance=str(manifest_path), qualified=False)
    target = out / "bluetooth-plan.json"
    build.save(target, plan)
    return target


def main():
    """Запускает подготовку и явную офлайн-сборку с отдельным результатом и без прошивки."""
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    for option in ("idf", "idf-tools", "toolchain", "python", "cmake", "ninja"):
        parser.add_argument(f"--{option}", type=Path)
    args = parser.parse_args()
    plan = prepare(args)
    return build.execute(plan, args, config_check=verify_profile)


if __name__ == "__main__":
    raise SystemExit(main())

