// Build 267. Evidence: D:/Games-cache/royale/reports/spatial-native-proof.py.
// VU setter 10A8D260 calls vtable+54 after its typed weak lookup. Stock client
// +50/+54 = 01191180/011911C0; dedicated +58/+5C = 00843530/00843570.
// The native setter copies old/new transforms and calls onTransformChanged
// (+A4 client / +AC dedicated) with old transform and notification flag true.
// Never replace this operation with a write to SpatialEntity's storage.
#include "NativeSpatial.h"
#include "EngineResources.h"
#include "EVBotsLog.h"
#include <cmath>
#include <cstring>

namespace evbots { namespace spatial { namespace {
using Ptr = std::uintptr_t;
struct Layout { Ptr getter, setter, implementation; unsigned getterSlot, changedSlot; };
constexpr Layout kClient = {0x01191180,0x011911C0,0x0118C980,0x50,0xA4};
constexpr Layout kServer = {0x00843530,0x00843570,0x0083CF80,0x58,0xAC};
constexpr Ptr kServerLocatorType=0x02BE7AE8, kServerLocatorVtable=0x02472C20;
constexpr Ptr kServerLocatorGetType=0x005E46C0;
constexpr Ptr kServerLocatorGetter=0x005E46F0, kServerLocatorSetter=0x005EDBB0;
constexpr Ptr kServerLocatorDispatch=0x004E1830;
// LocatorEntity stores a 64-byte transform inline at +20. Its override also
// writes the connected transform output at +60 and dispatches its data event.
constexpr unsigned char kLocatorGetterBytes[] = {
 0x56,0x57,0x8B,0x7C,0x24,0x0C,0x8D,0x71,0x20,0xB9,0x10,0,0,0,0xF3,0xA5,
 0x5F,0x5E,0xC2,0x04,0
};
constexpr unsigned char kLocatorSetterBytes[] = {
 0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x48,0x56,0x8B,0x75,0x08,0x8B,0xC1,0x57,
 0xB9,0x10,0,0,0,0x8D,0x7C,0x24,0x10,0xF3,0xA5,0x8D,0x50,0x20,0x8B,0x40,
 0x60,0xB9,0x10,0,0,0,0x8D,0x74,0x24,0x10,0x8B,0xFA,0xC7,0x44,0x24,0x4C,
 0xAF,0xAF,0xAF,0xAF,0xF3,0xA5,0x85,0xC0,0x74,0x1C,0x8B,0x38,0x8B,0xCF,0x83,
 0xC9,0x03,0x89,0x08,0x83,0xE7,0xFC,0x8B,0xF2,0xB9,0x10,0,0,0,0xF3,0xA5,
 0x8B,0xC8,0xE8,0x2A,0x3C,0xEF,0xFF,0x5F,0x5E,0x8B,0xE5,0x5D,0xC2,0x04,0
};
constexpr unsigned char kLocatorDispatchHead[] = {
 0x81,0xEC,0x1C,0x08,0,0,0x53,0x56,0x8B,0xF1,0x8B,0x5E,0x04,0xF6,0xC3,0x01,
 0x0F,0x84,0xB5,0,0,0
};
static_assert(sizeof(kLocatorGetterBytes)==0x15 && sizeof(kLocatorSetterBytes)==0x5E,"native LocatorEntity instruction boundaries");
// Complete functions, verified with Capstone from both shipped PE files. None
// contain absolute addresses; relocation only changes the vtable pointer values.
constexpr unsigned char kGetter[] = {
 0x8B,0x41,0x08,0xC1,0xE8,0x12,0x56,0x57,0xA8,0x01,0x74,0x21,0x8B,0x41,0x1C,0x0F,
 0xB6,0x48,0x06,0x8B,0x7C,0x24,0x0C,0x8D,0x0C,0x89,0xC1,0xE1,0x04,0x8D,0x74,0x01,
 0x10,0xB9,0x10,0,0,0,0xF3,0xA5,0x5F,0x5E,0xC2,0x04,0,0x8B,0x71,0x1C,0x8B,0x7C,
 0x24,0x0C,0xB9,0x10,0,0,0,0xF3,0xA5,0x5F,0x5E,0xC2,0x04,0
};
constexpr unsigned char kImplementation[] = {
 0x55,0x8B,0xEC,0x83,0xE4,0xF0,0x83,0xEC,0x44,0x53,0x8B,0xD1,0x8B,0x5A,0x08,0x8B,
 0x42,0x1C,0xC1,0xEB,0x12,0x80,0xE3,0x01,0x56,0x57,0x74,0x10,0x0F,0xB6,0x48,0x06,
 0x8D,0x0C,0x89,0xC1,0xE1,0x04,0x8D,0x74,0x01,0x10,0xEB,0x02,0x8B,0xF0,0xB9,0x10,
 0,0,0,0x8D,0x7C,0x24,0x10,0xF3,0xA5,0x84,0xDB,0x74,0x0E,0x0F,0xB6,0x48,0x06,
 0x8D,0x0C,0x89,0xC1,0xE1,0x04,0x8D,0x44,0x01,0x10,0x8B,0x75,0x08,0x8B,0xF8,0xB9,
 0x10,0,0,0,0xF3,0xA5,0x8B,0x4D,0x0C,0x8B,0x02,0x51,0x8D,0x4C,0x24,0x14,0x51,
 0x8B,0xCA,0x8B,0x90,0xA4,0,0,0,0xFF,0xD2,0x5F,0x5E,0x5B,0x8B,0xE5,0x5D,0xC2,0x08,0
};
static_assert(sizeof(kGetter)==64 && sizeof(kImplementation)==117,"native SpatialEntity instruction boundaries");

bool Fail(const char** error, const char* message) { if(error)*error=message; return false; }
bool WriteDisabled() {
 char flag[4]={}; return EnvGetCached("EVBOTS_SPATIAL_TRANSFORM",flag,sizeof(flag))==1 && flag[0]=='0';
}
Ptr Va(Ptr base,Ptr address) { return base+address-0x00400000; }
bool ImageRange(Ptr address, std::size_t size, Ptr base, bool code) {
 if(!address || !size || address+size<address) return false;
 MEMORY_BASIC_INFORMATION page={};
 if(!VirtualQuery(reinterpret_cast<const void*>(address),&page,sizeof(page)) || page.State!=MEM_COMMIT ||
    page.Type!=MEM_IMAGE || reinterpret_cast<Ptr>(page.AllocationBase)!=base ||
    (page.Protect&(PAGE_GUARD|PAGE_NOACCESS)) ||
    address+size>reinterpret_cast<Ptr>(page.BaseAddress)+page.RegionSize) return false;
 const DWORD protection=page.Protect&0xFF;
 if(code) return protection==PAGE_EXECUTE || protection==PAGE_EXECUTE_READ ||
                 protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY;
 return protection==PAGE_READONLY || protection==PAGE_READWRITE || protection==PAGE_WRITECOPY ||
        protection==PAGE_EXECUTE_READ || protection==PAGE_EXECUTE_READWRITE || protection==PAGE_EXECUTE_WRITECOPY;
}
bool CodeMatches(Ptr base,const Layout& layout) {
 __try {
  unsigned char setter[]={0x8B,0x44,0x24,0x04,0x6A,0x01,0x50,0xE8,0,0,0,0,0xC2,0x04,0};
  const std::int32_t relative=static_cast<std::int32_t>(layout.implementation-layout.setter-12);
  std::memcpy(setter+8,&relative,sizeof(relative));
  unsigned char implementation[sizeof(kImplementation)];
  std::memcpy(implementation,kImplementation,sizeof(implementation));
  implementation[102]=static_cast<unsigned char>(layout.changedSlot);
  return ImageRange(Va(base,layout.getter),sizeof(kGetter),base,true) &&
         ImageRange(Va(base,layout.setter),sizeof(setter),base,true) &&
         ImageRange(Va(base,layout.implementation),sizeof(implementation),base,true) &&
         std::memcmp(reinterpret_cast<const void*>(Va(base,layout.getter)),kGetter,sizeof(kGetter))==0 &&
         std::memcmp(reinterpret_cast<const void*>(Va(base,layout.setter)),setter,sizeof(setter))==0 &&
         std::memcmp(reinterpret_cast<const void*>(Va(base,layout.implementation)),implementation,sizeof(implementation))==0;
 } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool LocatorCodeMatches(Ptr base,Ptr type,Ptr table,Ptr getter,Ptr setter) {
 if(type!=Va(base,kServerLocatorType) || table!=Va(base,kServerLocatorVtable) ||
    getter!=Va(base,kServerLocatorGetter) || setter!=Va(base,kServerLocatorSetter))return false;
 const Ptr getType=Va(base,kServerLocatorGetType), dispatch=Va(base,kServerLocatorDispatch);
 unsigned char typeGetter[]={0xB8,0,0,0,0,0xC3};
 const auto relocatedType=static_cast<std::uint32_t>(type);
 std::memcpy(typeGetter+1,&relocatedType,sizeof(relocatedType));
 return *reinterpret_cast<const Ptr*>(table)==getType &&
        ImageRange(getType,sizeof(typeGetter),base,true) &&
        ImageRange(getter,sizeof(kLocatorGetterBytes),base,true) &&
        ImageRange(setter,sizeof(kLocatorSetterBytes),base,true) &&
        ImageRange(dispatch,sizeof(kLocatorDispatchHead),base,true) &&
        std::memcmp(reinterpret_cast<const void*>(getType),typeGetter,sizeof(typeGetter))==0 &&
        std::memcmp(reinterpret_cast<const void*>(getter),kLocatorGetterBytes,sizeof(kLocatorGetterBytes))==0 &&
        std::memcmp(reinterpret_cast<const void*>(setter),kLocatorSetterBytes,sizeof(kLocatorSetterBytes))==0 &&
        std::memcmp(reinterpret_cast<const void*>(dispatch),kLocatorDispatchHead,sizeof(kLocatorDispatchHead))==0;
}
struct Operation { alignas(16) float matrix[16]; bool write; };
bool Finite(const float* matrix) {
 for(unsigned row=0;row<4;++row) for(unsigned col=0;col<3;++col)
  if(!std::isfinite(matrix[row*4+col]))return false;
 return true;
}

// Invoked only inside Blueprint::WithEntityContext after its weak-generation,
// frame, native TLS, level, bus, pending-destruction and vtable checks.
bool Operate(Ptr entity,void* opaque,const char** error) {
 auto* operation=static_cast<Operation*>(opaque);
 const bool client=blueprint::IsClient();
 const auto& layout=client?kClient:kServer;
 HMODULE module=GetModuleHandleW(client?L"bf3.exe":L"bf3_server_Final.exe");
 if(client && !module)module=GetModuleHandleW(L"zbf3.exe");
 const Ptr base=reinterpret_cast<Ptr>(module);
 if(!base || !CodeMatches(base,layout))return Fail(error,"SpatialEntity native transform signatures do not match this engine");
 const auto type=eng::TypeOf(static_cast<unsigned long>(entity));
 if(!eng::IsKindOfName(type,"SpatialEntity"))return Fail(error,"entity is not a native SpatialEntity");
 __try {
  const Ptr table=*reinterpret_cast<const Ptr*>(entity);
  if(!ImageRange(table,layout.changedSlot+sizeof(Ptr),base,false))
   return Fail(error,"SpatialEntity vtable is outside the native engine image");
  const Ptr getter=*reinterpret_cast<const Ptr*>(table+layout.getterSlot);
  const Ptr setter=*reinterpret_cast<const Ptr*>(table+layout.getterSlot+sizeof(Ptr));
  const bool stock=getter==Va(base,layout.getter) && setter==Va(base,layout.setter);
  if(!stock && (client || !LocatorCodeMatches(base,type,table,getter,setter)))
   return Fail(error,"SpatialEntity transform override is not verified for this entity");
  using Getter=void(__fastcall*)(void*,void*,float*);
  using Setter=void(__fastcall*)(void*,void*,const float*);
  if(operation->write) {
   if(stock) {
    const Ptr changed=*reinterpret_cast<const Ptr*>(table+layout.changedSlot);
    if(!ImageRange(changed,1,base,true))return Fail(error,"SpatialEntity onTransformChanged is not native executable code");
   }
   reinterpret_cast<Setter>(setter)(reinterpret_cast<void*>(entity),nullptr,operation->matrix);
  } else {
   reinterpret_cast<Getter>(getter)(reinterpret_cast<void*>(entity),nullptr,operation->matrix);
   if(!Finite(operation->matrix))return Fail(error,"SpatialEntity native transform is not finite");
  }
  return true;
 } __except(EXCEPTION_EXECUTE_HANDLER) {
  return Fail(error,operation->write ? "native SpatialEntity setter raised an exception; transform may have changed" :
                                      "native SpatialEntity getter raised an exception");
 }
}
} // namespace

bool GetTransform(const blueprint::Handle& entity,float* transform16,const char** error) {
 if(error)*error=nullptr;
 if(!transform16)return Fail(error,"missing transform output");
 Operation operation={};
 if(!blueprint::WithEntityContext(entity,Operate,&operation,error))return false;
 std::memcpy(transform16,operation.matrix,sizeof(operation.matrix));
 return true;
}
bool SetTransform(const blueprint::Handle& entity,const float* transform16,const char** error) {
 if(error)*error=nullptr;
 if(WriteDisabled())return Fail(error,"EVBOTS_SPATIAL_TRANSFORM=0");
 if(!transform16 || !Finite(transform16))return Fail(error,"SpatialEntity transform must contain finite vectors");
 Operation operation={}; operation.write=true;
 for(unsigned row=0;row<4;++row)for(unsigned col=0;col<3;++col)operation.matrix[row*4+col]=transform16[row*4+col];
 return blueprint::WithEntityContext(entity,Operate,&operation,error);
}
} }
