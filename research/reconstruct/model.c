
void *ModelComponent; // ComponentWithInterface

void *ModelInstance = *$(void*, ModelComponent, 0x48, 0x88);
// on 32 bit:
// boost::scoped_ptr<>::reset((scoped_ptr<> *)(this + 0x48),(ModelInstance *)0x0);
// This is a scoped pointer object?

void *SkeletonInstance; // *$(void*, ModelInstance, 0x0, 0x18);
void *Skeleton = //  *$(void*, SkeletonInstance, 0x0, 0x0);