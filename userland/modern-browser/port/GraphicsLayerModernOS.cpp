// This port paints LocalFrameView directly into a real CPU Skia surface.
// There is no default native compositor. Honor an actual embedder factory,
// and fail explicitly if a disabled compositing path asks for one.
#include "config.h"
#include "GraphicsLayer.h"
#include "GraphicsLayerFactory.h"

namespace WebCore {
Ref<GraphicsLayer> GraphicsLayer::create(GraphicsLayerFactory *factory, GraphicsLayerClient &client,
                                         Type type) {
    RELEASE_ASSERT_WITH_MESSAGE(factory, "ModernOS has no native graphics-layer compositor");
    return factory->createGraphicsLayer(type, client);
}
}
