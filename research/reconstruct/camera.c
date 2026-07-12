
// CameraController (_G.cameraController)

void *Camera; // *$(void*, CameraController, 0x54, 0x58)

// CameraController fields
int mode; // CameraController + (0x00, 0x00) 0=idle, 1=following, 2=transitioning
Vector3 offset; // CameraController + (0x04, 0x04)
float zoomOffset; // CameraController + (0x0c, 0x0c)
Vector3 targetPosition; // CameraController + (0x10, 0x10)
Vector3 targetLookAt; // CameraController + (0x20, 0x20)
float targetZoom; // CameraController + (0x18, 0x18)
float positionSmoothing; // CameraController + (0x1c, 0x1c)
float lookAtSmoothing; // CameraController + (0x2c, 0x2c)
Vector3 currentPosition; // CameraController + (0x30, 0x30)
Vector3 currentLookAt; // CameraController + (0x3c, 0x3c)
float currentZoom; // CameraController + (0x38, 0x38)
float currentLookAtZoom; // CameraController + (0x44, 0x44)
Vector3 upVector; // CameraController + (0x48, 0x48)
// *Camera at 0x54, 0x58
void *camera; // CameraController + (0x54, 0x58)
void *followedEntity; // CameraController + (0x5c, 0x68)
void *focusShape; // CameraController + (0x60, 0x70)
Vector3 followOffset; // CameraController + (0x64, 0x78) added to entity position
float followZoomOffset; // CameraController + (0x70, 0x80) (32-bit: 100 decimal?)
// Rectangle focusRect at (0x70, 0x84)
float shakeElapsed; // CameraController + (0x80, 0x94)
float shakeDuration; // CameraController + (0x84, 0x98)

// Camera fields
bool isOrtho; // Camera + (0xec, 0xec)
float aspectRatio; // Camera + (0xf0, 0xf0)
float fov; // Camera + (0xf4, 0xf4)
float farClip; // Camera + (0xfc, 0xfc)
// Matrix4 projectionMatrix at (0x6c, 0x6c) 64 bytes
// Matrix4 invProjectionMatrix at (0x100, 0x100) 64 bytes
// Matrix4 viewProjMatrix at (0xac, 0xac) used in screen/world conversions???