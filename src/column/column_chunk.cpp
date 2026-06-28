#include "column/column_chunk.h"

// ChunkMeta is a plain-old-data header; all behaviour lives in the writer and
// reader. This translation unit exists to anchor the header in the build and
// to host any future chunk-level helpers.

namespace liteolap {}  // namespace liteolap
