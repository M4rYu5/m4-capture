#include "dmabuf-fixture.hpp"

#include <atomic>
#include <unordered_set>

namespace
{
std::atomic<uint64_t> created{0}, destroyed{0};
// OBS serializes all texture operations through its graphics context.
std::unordered_set<gs_texture_t *> imports;
} // namespace

DmaBufImportStats dmabuf_import_stats() { return {created.load(), destroyed.load()}; }

extern "C" gs_texture_t *__real_gs_texture_create_from_dmabuf(unsigned int width, unsigned int height, uint32_t format,
							      gs_color_format color, uint32_t planes, const int *fds,
							      const uint32_t *strides, const uint32_t *offsets,
							      const uint64_t *modifiers);
extern "C" void __real_gs_texture_destroy(gs_texture_t *texture);

extern "C" gs_texture_t *__wrap_gs_texture_create_from_dmabuf(unsigned int width, unsigned int height, uint32_t format,
							      gs_color_format color, uint32_t planes, const int *fds,
							      const uint32_t *strides, const uint32_t *offsets,
							      const uint64_t *modifiers)
{
	auto texture = __real_gs_texture_create_from_dmabuf(width, height, format, color, planes, fds, strides, offsets,
							    modifiers);
	if (texture) {
		imports.insert(texture);
		++created;
	}
	return texture;
}

extern "C" void __wrap_gs_texture_destroy(gs_texture_t *texture)
{
	if (imports.erase(texture))
		++destroyed;
	__real_gs_texture_destroy(texture);
}
