#pragma once
#include <cstdint>
namespace nes::c6_colour {
/** @brief Два уже разрешённых CHR plane bytes и двухбитная палитра тайла; без ROM-ссылок. */
struct Tile { std::uint8_t low=0,high=0,attribute=0; };
static_assert(sizeof(Tile)==3,"explicit three-byte tile fields");
}
