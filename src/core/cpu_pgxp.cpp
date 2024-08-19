// SPDX-FileCopyrightText: 2016 iCatButler, 2019-2023 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: GPL-2.0+

#include "cpu_pgxp.h"
#include "bus.h"
#include "cpu_core.h"
#include "cpu_disasm.h"
#include "settings.h"

#include "util/gpu_device.h"

#include "common/assert.h"
#include "common/log.h"

#include <climits>
#include <cmath>

Log_SetChannel(CPU::PGXP);

// #define LOG_VALUES 1
// #define LOG_LOOKUPS 1

// TODO: Don't update flags on Validate(), instead return it.

namespace CPU::PGXP {
namespace {

enum : u32
{
  VERTEX_CACHE_WIDTH = 2048,
  VERTEX_CACHE_HEIGHT = 2048,
  VERTEX_CACHE_SIZE = VERTEX_CACHE_WIDTH * VERTEX_CACHE_HEIGHT,
  PGXP_MEM_SIZE = (static_cast<u32>(Bus::RAM_8MB_SIZE) + static_cast<u32>(CPU::SCRATCHPAD_SIZE)) / 4,
  PGXP_MEM_SCRATCH_OFFSET = Bus::RAM_8MB_SIZE / 4,
};

enum : u32
{
  COMP_X,
  COMP_Y,
  COMP_Z,
};

enum : u32
{
  VALID_X = (1u << 0),
  VALID_Y = (1u << 1),
  VALID_Z = (1u << 2),
  VALID_LOWZ = (1u << 16),      // Valid Z from the low part of a 32-bit value.
  VALID_HIGHZ = (1u << 17),     // Valid Z from the high part of a 32-bit value.
  VALID_TAINTED_Z = (1u << 31), // X/Y has been changed, Z may not be accurate.

  VALID_XY = (VALID_X | VALID_Y),
  VALID_XYZ = (VALID_X | VALID_Y | VALID_Z),
  VALID_ALL = (VALID_X | VALID_Y | VALID_Z),
};

union psx_value
{
  u32 d;
  s32 sd;
  struct
  {
    u16 l, h;
  } w;
  struct
  {
    s16 l, h;
  } sw;
};
} // namespace

static void CacheVertex(u32 value, const PGXP_value& vertex);
static PGXP_value* GetCachedVertex(u32 value);

static float TruncateVertexPosition(float p);
static bool IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y);

static void MakeValid(PGXP_value* pV, u32 psxV);

static void Validate(PGXP_value* pV, u32 psxV);
static void MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask);

static double f16Sign(double in);
static double f16Unsign(double in);
static double f16Overflow(double in);

static PGXP_value& GetRdValue(Instruction instr);
static PGXP_value& GetRtValue(Instruction instr);
static PGXP_value& ValidateAndGetRtValue(Instruction instr, u32 rtVal);
static PGXP_value& ValidateAndGetRsValue(Instruction instr, u32 rsVal);
static void SetRtValue(Instruction instr, const PGXP_value& val);
static void SetRtValue(Instruction instr, const PGXP_value& val, u32 rtVal);

static PGXP_value* GetPtr(u32 addr);

static const PGXP_value& ValidateAndLoadMem(u32 addr, u32 value);
static void ValidateAndLoadMem16(PGXP_value* dest, u32 addr, u32 value, bool sign);

static void CPU_MTC2_int(const PGXP_value& value, u32 reg, u32 val);
static void CPU_BITWISE(Instruction instr, u32 rdVal, u32 rsVal, u32 rtVal);

static void WriteMem(u32 addr, const PGXP_value& value);
static void WriteMem16(u32 addr, const PGXP_value& value);

static void CopyZIfMissing(PGXP_value& dst, const PGXP_value& src);
static void SelectZ(PGXP_value& dst, const PGXP_value& src1, const PGXP_value& src2);

#ifdef LOG_VALUES
static void LogInstruction(u32 pc, Instruction instr);
static void LogValue(const char* name, u32 rval, const PGXP_value* val);
static void LogValueStr(SmallStringBase& str, const char* name, u32 rval, const PGXP_value* val);

// clang-format off
#define LOG_VALUES_NV() do { LogInstruction(CPU::g_state.current_instruction_pc, instr); } while (0)
#define LOG_VALUES_1(name, rval, val) do { LogInstruction(CPU::g_state.current_instruction_pc, instr); LogValue(name, rval, val); } while (0)
#define LOG_VALUES_C1(rnum, rval) do { LogInstruction(CPU::g_state.current_instruction_pc,instr); LogValue(CPU::GetRegName(static_cast<CPU::Reg>(rnum)), rval, &g_state.pgxp_gpr[static_cast<u32>(rnum)]); } while(0)
#define LOG_VALUES_C2(r1num, r1val, r2num, r2val) do { LogInstruction(CPU::g_state.current_instruction_pc,instr); LogValue(CPU::GetRegName(static_cast<CPU::Reg>(r1num)), r1val, &g_state.pgxp_gpr[static_cast<u32>(r1num)]); LogValue(CPU::GetRegName(static_cast<CPU::Reg>(r2num)), r2val, &g_state.pgxp_gpr[static_cast<u32>(r2num)]); } while(0)
#define LOG_VALUES_LOAD(addr, val) do { LogInstruction(CPU::g_state.current_instruction_pc,instr); LogValue(TinyString::from_format("MEM[{:08X}]", addr).c_str(), val, GetPtr(addr)); } while(0)
#define LOG_VALUES_STORE(rnum, rval, addr) do { LOG_VALUES_C1(rnum, rval); std::fprintf(s_log, " addr=%08X", addr); } while(0)
#else
#define LOG_VALUES_NV() (void)0
#define LOG_VALUES_1(name, rval, val) (void)0
#define LOG_VALUES_C1(rnum, rval) (void)0
#define LOG_VALUES_C2(r1num, r1val, r2num, r2val) (void)0
#define LOG_VALUES_LOAD(addr, val) (void)0
#define LOG_VALUES_STORE(rnum, rval, addr) (void)0
#endif
// clang-format on

static constexpr PGXP_value PGXP_value_invalid = {0.f, 0.f, 0.f, 0, 0};
static constexpr PGXP_value PGXP_value_zero = {0.f, 0.f, 0.f, 0, VALID_XY};

static PGXP_value* s_mem = nullptr;
static PGXP_value* s_vertex_cache = nullptr;

#ifdef LOG_VALUES
static std::FILE* s_log;
#endif
} // namespace CPU::PGXP

void CPU::PGXP::Initialize()
{
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));

  if (!s_mem)
  {
    s_mem = static_cast<PGXP_value*>(std::calloc(PGXP_MEM_SIZE, sizeof(PGXP_value)));
    if (!s_mem)
      Panic("Failed to allocate PGXP memory");
  }

  if (g_settings.gpu_pgxp_vertex_cache && !s_vertex_cache)
  {
    s_vertex_cache = static_cast<PGXP_value*>(std::calloc(VERTEX_CACHE_SIZE, sizeof(PGXP_value)));
    if (!s_vertex_cache)
    {
      ERROR_LOG("Failed to allocate memory for vertex cache, disabling.");
      g_settings.gpu_pgxp_vertex_cache = false;
    }
  }

  if (s_vertex_cache)
    std::memset(s_vertex_cache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);
}

void CPU::PGXP::Reset()
{
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));

  if (s_mem)
    std::memset(s_mem, 0, sizeof(PGXP_value) * PGXP_MEM_SIZE);

  if (g_settings.gpu_pgxp_vertex_cache && s_vertex_cache)
    std::memset(s_vertex_cache, 0, sizeof(PGXP_value) * VERTEX_CACHE_SIZE);
}

void CPU::PGXP::Shutdown()
{
  if (s_vertex_cache)
  {
    std::free(s_vertex_cache);
    s_vertex_cache = nullptr;
  }
  if (s_mem)
  {
    std::free(s_mem);
    s_mem = nullptr;
  }

  std::memset(g_state.pgxp_gte, 0, sizeof(g_state.pgxp_gte));
  std::memset(g_state.pgxp_gpr, 0, sizeof(g_state.pgxp_gpr));
  std::memset(g_state.pgxp_cop0, 0, sizeof(g_state.pgxp_cop0));
}

#define SX0 (g_state.pgxp_gte[12].x)
#define SY0 (g_state.pgxp_gte[12].y)
#define SX1 (g_state.pgxp_gte[13].x)
#define SY1 (g_state.pgxp_gte[13].y)
#define SX2 (g_state.pgxp_gte[14].x)
#define SY2 (g_state.pgxp_gte[14].y)

#define SXY0 (g_state.pgxp_gte[12])
#define SXY1 (g_state.pgxp_gte[13])
#define SXY2 (g_state.pgxp_gte[14])
#define SXYP (g_state.pgxp_gte[15])

ALWAYS_INLINE_RELEASE void CPU::PGXP::MakeValid(PGXP_value* pV, u32 psxV)
{
  if ((pV->flags & VALID_XY) == VALID_XY)
    return;

  pV->x = static_cast<float>(static_cast<s16>(Truncate16(psxV)));
  pV->y = static_cast<float>(static_cast<s16>(Truncate16(psxV >> 16)));
  pV->z = 0.0f;
  pV->flags = VALID_XY | VALID_TAINTED_Z;
  pV->value = psxV;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::Validate(PGXP_value* pV, u32 psxV)
{
  pV->flags = (pV->value == psxV) ? pV->flags : 0;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::MaskValidate(PGXP_value* pV, u32 psxV, u32 mask, u32 validMask)
{
  pV->flags = ((pV->value & mask) == (psxV & mask)) ? pV->flags : (pV->flags & ~validMask);
}

ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Sign(double in)
{
  const s32 s = static_cast<s32>(static_cast<s64>(in * (USHRT_MAX + 1)));
  return static_cast<double>(s) / static_cast<double>(USHRT_MAX + 1);
}
ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Unsign(double in)
{
  return (in >= 0) ? in : (in + (USHRT_MAX + 1));
}
ALWAYS_INLINE_RELEASE double CPU::PGXP::f16Overflow(double in)
{
  double out = 0;
  s64 v = ((s64)in) >> 16;
  out = (double)v;
  return out;
}

ALWAYS_INLINE CPU::PGXP_value& CPU::PGXP::GetRdValue(Instruction instr)
{
  return g_state.pgxp_gpr[static_cast<u8>(instr.r.rd.GetValue())];
}

ALWAYS_INLINE CPU::PGXP_value& CPU::PGXP::GetRtValue(Instruction instr)
{
  return g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())];
}

ALWAYS_INLINE CPU::PGXP_value& CPU::PGXP::ValidateAndGetRtValue(Instruction instr, u32 rtVal)
{
  PGXP_value& ret = g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())];
  Validate(&ret, rtVal);
  return ret;
}

ALWAYS_INLINE CPU::PGXP_value& CPU::PGXP::ValidateAndGetRsValue(Instruction instr, u32 rsVal)
{
  PGXP_value& ret = g_state.pgxp_gpr[static_cast<u8>(instr.r.rs.GetValue())];
  Validate(&ret, rsVal);
  return ret;
}

ALWAYS_INLINE void CPU::PGXP::SetRtValue(Instruction instr, const PGXP_value& val)
{
  g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())] = val;
}

ALWAYS_INLINE void CPU::PGXP::SetRtValue(Instruction instr, const PGXP_value& val, u32 rtVal)
{
  PGXP_value& prtVal = g_state.pgxp_gpr[static_cast<u8>(instr.r.rt.GetValue())];
  prtVal = val;
  prtVal.value = rtVal;
}

ALWAYS_INLINE_RELEASE CPU::PGXP_value* CPU::PGXP::GetPtr(u32 addr)
{
#if 0
  if ((addr & CPU::PHYSICAL_MEMORY_ADDRESS_MASK) >= 0x0017A2B4 &&
      (addr & CPU::PHYSICAL_MEMORY_ADDRESS_MASK) <= 0x0017A2B4)
    __debugbreak();
#endif

  if ((addr & SCRATCHPAD_ADDR_MASK) == SCRATCHPAD_ADDR)
    return &s_mem[PGXP_MEM_SCRATCH_OFFSET + ((addr & SCRATCHPAD_OFFSET_MASK) >> 2)];

  const u32 paddr = (addr & PHYSICAL_MEMORY_ADDRESS_MASK);
  if (paddr < Bus::RAM_MIRROR_END)
    return &s_mem[(paddr & Bus::g_ram_mask) >> 2];
  else
    return nullptr;
}

ALWAYS_INLINE_RELEASE const CPU::PGXP_value& CPU::PGXP::ValidateAndLoadMem(u32 addr, u32 value)
{
  PGXP_value* pMem = GetPtr(addr);
  if (!pMem) [[unlikely]]
    return PGXP_value_invalid;

  Validate(pMem, value);
  return *pMem;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::ValidateAndLoadMem16(PGXP_value* dest, u32 addr, u32 value, bool sign)
{
  PGXP_value* pMem = GetPtr(addr);
  if (!pMem)
  {
    *dest = PGXP_value_invalid;
    return;
  }

  psx_value val{0}, mask{0};
  u32 valid_mask = 0;

  // determine if high or low word
  const bool hiword = ((addr & 2) != 0);
  if (hiword)
  {
    val.w.h = static_cast<u16>(value);
    mask.w.h = 0xFFFF;
    valid_mask = VALID_Y;
  }
  else
  {
    val.w.l = static_cast<u16>(value);
    mask.w.l = 0xFFFF;
    valid_mask = VALID_X;
  }

  // validate and copy whole value
  MaskValidate(pMem, val.d, mask.d, valid_mask);
  *dest = *pMem;

  // if high word then shift
  if (hiword)
  {
    dest->x = dest->y;
    dest->SetValid(COMP_X, dest->HasValid(COMP_Y));
  }

  // only set y as valid if x is also valid.. don't want to make fake values
  if (dest->HasValid(COMP_X))
  {
    dest->y = (dest->x < 0) ? -1.f * sign : 0.f;
    dest->SetValid(COMP_Y);
  }
  else
  {
    dest->y = 0.0f;
    dest->SetValid(COMP_Y, false);
  }

  dest->value = value;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::WriteMem(u32 addr, const PGXP_value& value)
{
  PGXP_value* pMem = GetPtr(addr);
  if (!pMem) [[unlikely]]
    return;

  *pMem = value;
  pMem->flags |= VALID_LOWZ | VALID_HIGHZ;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::WriteMem16(u32 addr, const PGXP_value& value)
{
  PGXP_value* dest = GetPtr(addr);
  if (!dest) [[unlikely]]
    return;

  // determine if high or low word
  const bool hiword = ((addr & 2) != 0);
  if (hiword)
  {
    dest->y = value.x;
    dest->SetValid(COMP_Y, value.HasValid(COMP_X));
    dest->value = (dest->value & UINT32_C(0x0000FFFF)) | (value.value << 16);
  }
  else
  {
    dest->x = value.x;
    dest->SetValid(COMP_X, value.HasValid(COMP_X));
    dest->value = (dest->value & UINT32_C(0xFFFF0000)) | (value.value & UINT32_C(0x0000FFFF));
  }

  // overwrite z/w if valid
  // TODO: Check modified
  if (value.HasValid(COMP_Z))
  {
    dest->z = value.z;
    dest->SetValid(COMP_Z);
    dest->flags |= hiword ? VALID_HIGHZ : VALID_LOWZ;
  }
  else
  {
    dest->flags &= hiword ? ~VALID_HIGHZ : ~VALID_LOWZ;
    if (dest->flags & VALID_Z && !(dest->flags & (VALID_HIGHZ | VALID_LOWZ)))
      dest->flags &= ~VALID_Z;
  }
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CopyZIfMissing(PGXP_value& dst, const PGXP_value& src)
{
  dst.z = dst.HasValid(COMP_Z) ? dst.z : src.z;
  dst.flags |= (src.flags & VALID_Z);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::SelectZ(PGXP_value& dst, const PGXP_value& src1, const PGXP_value& src2)
{
  // Prefer src2 if src1 is missing Z, or is potentially an imprecise value, when src2 is precise.
  dst.z = (!(src1.flags & VALID_Z) ||
           (src1.flags & VALID_TAINTED_Z && (src2.flags & (VALID_Z | VALID_TAINTED_Z)) == VALID_Z)) ?
            src2.z :
            src1.z;
  dst.flags |= ((src1.flags | src2.flags) & VALID_Z);
}

#ifdef LOG_VALUES
void CPU::PGXP::LogInstruction(u32 pc, Instruction instr)
{
  if (!s_log) [[unlikely]]
  {
    s_log = std::fopen("pgxp.log", "wb");
  }
  else
  {
    std::fflush(s_log);
    std::fputc('\n', s_log);
  }

  SmallString str;
  DisassembleInstruction(&str, pc, instr.bits);
  std::fprintf(s_log, "%08X %08X %-20s", pc, instr.bits, str.c_str());
}

void CPU::PGXP::LogValue(const char* name, u32 rval, const PGXP_value* val)
{
  if (!s_log) [[unlikely]]
    return;

  SmallString str;
  LogValueStr(str, name, rval, val);
  std::fprintf(s_log, " %s", str.c_str());
}

void CPU::PGXP::LogValueStr(SmallStringBase& str, const char* name, u32 rval, const PGXP_value* val)
{
  str.append_format("{}=[{:08X}", name, rval);
  if (!val)
  {
    str.append(", NULL]");
  }
  else
  {
    if (val->value != rval)
      str.append_format(", PGXP{:08X}", val->value);

    str.append_format(", {{{},{},{}}}", val->x, val->y, val->z);

    if (val->flags & VALID_ALL)
    {
      str.append(", valid=");
      if (val->flags & VALID_X)
        str.append('X');
      if (val->flags & VALID_Y)
        str.append('Y');
      if (val->flags & VALID_Z)
        str.append('Z');
    }

    // if (val->flags & VALID_TAINTED_Z)
    // str.append(", tainted");

    str.append(']');
  }
}

#endif

void CPU::PGXP::GTE_RTPS(float x, float y, float z, u32 value)
{
  // push values down FIFO
  SXY0 = SXY1;
  SXY1 = SXY2;

  SXY2.x = x;
  SXY2.y = y;
  SXY2.z = z;
  SXY2.value = value;
  SXY2.flags = VALID_ALL;

  if (g_settings.gpu_pgxp_vertex_cache)
    CacheVertex(value, SXY2);
}

#define VX(n) (psxRegs.CP2D.p[n << 1].sw.l)
#define VY(n) (psxRegs.CP2D.p[n << 1].sw.h)
#define VZ(n) (psxRegs.CP2D.p[(n << 1) + 1].sw.l)

int CPU::PGXP::GTE_NCLIP_valid(u32 sxy0, u32 sxy1, u32 sxy2)
{
  Validate(&SXY0, sxy0);
  Validate(&SXY1, sxy1);
  Validate(&SXY2, sxy2);

  // Don't use accurate clipping for game-constructed values, which don't have a valid Z.
  return (((SXY0.flags & SXY1.flags & SXY2.flags & VALID_XYZ) == VALID_XYZ));
}

float CPU::PGXP::GTE_NCLIP()
{
  float nclip = ((SX0 * SY1) + (SX1 * SY2) + (SX2 * SY0) - (SX0 * SY2) - (SX1 * SY0) - (SX2 * SY1));

  // ensure fractional values are not incorrectly rounded to 0
  float nclipAbs = std::abs(nclip);
  if ((0.1f < nclipAbs) && (nclipAbs < 1.f))
    nclip += (nclip < 0.f ? -1 : 1);

  // float AX = SX1 - SX0;
  // float AY = SY1 - SY0;

  // float BX = SX2 - SX0;
  // float BY = SY2 - SY0;

  //// normalise A and B
  // float mA = sqrt((AX*AX) + (AY*AY));
  // float mB = sqrt((BX*BX) + (BY*BY));

  //// calculate AxB to get Z component of C
  // float CZ = ((AX * BY) - (AY * BX)) * (1 << 12);

  return nclip;
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_MTC2_int(const PGXP_value& value, u32 reg, u32 val)
{
  switch (reg)
  {
    case 15:
      // push FIFO
      SXY0 = SXY1;
      SXY1 = SXY2;
      SXY2 = value;
      SXYP = SXY2;
      break;

    case 31:
      return;
  }

  PGXP_value& gteVal = g_state.pgxp_gte[reg];
  gteVal = value;
  gteVal.value = val;
}

////////////////////////////////////
// Data transfer tracking
////////////////////////////////////

void CPU::PGXP::CPU_MFC2(Instruction instr, u32 rdVal)
{
  // CPU[Rt] = GTE_D[Rd]
  const u32 idx = instr.cop.Cop2Index();
  LOG_VALUES_1(CPU::GetGTERegisterName(idx), rdVal, &g_state.pgxp_gte[idx]);

  PGXP_value& prdVal = g_state.pgxp_gte[idx];
  Validate(&prdVal, rdVal);
  SetRtValue(instr, prdVal, rdVal);
}

void CPU::PGXP::CPU_MTC2(Instruction instr, u32 rtVal)
{
  // GTE_D[Rd] = CPU[Rt]
  const u32 idx = instr.cop.Cop2Index();
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  CPU_MTC2_int(prtVal, idx, rtVal);
}

////////////////////////////////////
// Memory Access
////////////////////////////////////
void CPU::PGXP::CPU_LWC2(Instruction instr, u32 addr, u32 rtVal)
{
  // GTE_D[Rt] = Mem[addr]
  LOG_VALUES_LOAD(addr, rtVal);

  const PGXP_value& pMem = ValidateAndLoadMem(addr, rtVal);
  CPU_MTC2_int(pMem, static_cast<u32>(instr.r.rt.GetValue()), rtVal);
}

void CPU::PGXP::CPU_SWC2(Instruction instr, u32 addr, u32 rtVal)
{
  //  Mem[addr] = GTE_D[Rt]
  const u32 idx = static_cast<u32>(instr.r.rt.GetValue());
  PGXP_value& prtVal = g_state.pgxp_gte[idx];
#ifdef LOG_VALUES
  LOG_VALUES_1(CPU::GetGTERegisterName(idx), rtVal, &prtVal);
  std::fprintf(s_log, " addr=%08X", addr);
#endif
  Validate(&prtVal, rtVal);
  WriteMem(addr, prtVal);
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CacheVertex(u32 value, const PGXP_value& vertex)
{
  const s16 sx = static_cast<s16>(value & 0xFFFFu);
  const s16 sy = static_cast<s16>(value >> 16);
  DebugAssert(sx >= -1024 && sx <= 1023 && sy >= -1024 && sy <= 1023);
  s_vertex_cache[(sy + 1024) * VERTEX_CACHE_WIDTH + (sx + 1024)] = vertex;
}

ALWAYS_INLINE_RELEASE CPU::PGXP_value* CPU::PGXP::GetCachedVertex(u32 value)
{
  const s16 sx = static_cast<s16>(value & 0xFFFFu);
  const s16 sy = static_cast<s16>(value >> 16);
  return (sx >= -1024 && sx <= 1023 && sy >= -1024 && sy <= 1013) ?
           &s_vertex_cache[(sy + 1024) * VERTEX_CACHE_WIDTH + (sx + 1024)] :
           nullptr;
}

ALWAYS_INLINE_RELEASE float CPU::PGXP::TruncateVertexPosition(float p)
{
  const s32 int_part = static_cast<s32>(p);
  const float int_part_f = static_cast<float>(int_part);
  return static_cast<float>(static_cast<s16>(int_part << 5) >> 5) + (p - int_part_f);
}

ALWAYS_INLINE_RELEASE bool CPU::PGXP::IsWithinTolerance(float precise_x, float precise_y, int int_x, int int_y)
{
  const float tolerance = g_settings.gpu_pgxp_tolerance;
  if (tolerance < 0.0f)
    return true;

  return (std::abs(precise_x - static_cast<float>(int_x)) <= tolerance &&
          std::abs(precise_y - static_cast<float>(int_y)) <= tolerance);
}

bool CPU::PGXP::GetPreciseVertex(u32 addr, u32 value, int x, int y, int xOffs, int yOffs, float* out_x, float* out_y,
                                 float* out_w)
{
  const PGXP_value* vert = GetPtr(addr);
  if (vert && ((vert->flags & VALID_XY) == VALID_XY) && (vert->value == value))
  {
    // There is a value here with valid X and Y coordinates
    *out_x = TruncateVertexPosition(vert->x) + static_cast<float>(xOffs);
    *out_y = TruncateVertexPosition(vert->y) + static_cast<float>(yOffs);
    *out_w = vert->z / 32768.0f;

#ifdef LOG_LOOKUPS
    GL_INS_FMT("0x{:08X} {},{} => {},{} ({},{},{}) ({},{})", addr, x, y, *out_x, *out_y,
               TruncateVertexPosition(vert->x), TruncateVertexPosition(vert->y), vert->z, std::abs(*out_x - x),
               std::abs(*out_y - y));
#endif

    if (IsWithinTolerance(*out_x, *out_y, x, y))
    {
      // check validity of z component
      return ((vert->flags & VALID_Z) == VALID_Z);
    }
  }

  if (g_settings.gpu_pgxp_vertex_cache)
  {
    vert = GetCachedVertex(value);
    if (vert && (vert->flags & VALID_XY) == VALID_XY)
    {
      *out_x = TruncateVertexPosition(vert->x) + static_cast<float>(xOffs);
      *out_y = TruncateVertexPosition(vert->y) + static_cast<float>(yOffs);
      *out_w = vert->z / 32768.0f;

      if (IsWithinTolerance(*out_x, *out_y, x, y))
        return false;
    }
  }

  // no valid value can be found anywhere, use the native PSX data
  *out_x = static_cast<float>(x);
  *out_y = static_cast<float>(y);
  *out_w = 1.0f;
  return false;
}

void CPU::PGXP::CPU_LW(Instruction instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im]
  LOG_VALUES_LOAD(addr, rtVal);
  SetRtValue(instr, ValidateAndLoadMem(addr, rtVal));
}

void CPU::PGXP::CPU_LBx(Instruction instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_LOAD(addr, rtVal);
  SetRtValue(instr, PGXP_value_invalid);
}

void CPU::PGXP::CPU_LH(Instruction instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im] (sign extended)
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndLoadMem16(&GetRtValue(instr), addr, rtVal, true);
}

void CPU::PGXP::CPU_LHU(Instruction instr, u32 addr, u32 rtVal)
{
  // Rt = Mem[Rs + Im] (zero extended)
  LOG_VALUES_LOAD(addr, rtVal);
  ValidateAndLoadMem16(&GetRtValue(instr), addr, rtVal, false);
}

void CPU::PGXP::CPU_SB(Instruction instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_STORE(instr.r.rt.GetValue(), rtVal, addr);
  WriteMem(addr, PGXP_value_invalid);
}

void CPU::PGXP::CPU_SH(Instruction instr, u32 addr, u32 rtVal)
{
  LOG_VALUES_STORE(instr.r.rt.GetValue(), rtVal, addr);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  WriteMem16(addr, prtVal);
}

void CPU::PGXP::CPU_SW(Instruction instr, u32 addr, u32 rtVal)
{
  // Mem[Rs + Im] = Rt
  LOG_VALUES_STORE(instr.r.rt.GetValue(), rtVal, addr);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  WriteMem(addr, prtVal);
}

void CPU::PGXP::CPU_MOVE_Packed(u32 rd_and_rs, u32 rsVal)
{
  const u32 Rs = (rd_and_rs & 0xFFu);
  const u32 Rd = (rd_and_rs >> 8);
  CPU_MOVE(Rd, Rs, rsVal);
}

void CPU::PGXP::CPU_MOVE(u32 Rd, u32 Rs, u32 rsVal)
{
#ifdef LOG_VALUES
  const Instruction instr = {0};
  LOG_VALUES_C1(Rs, rsVal);
#endif
  Validate(&g_state.pgxp_gpr[Rs], rsVal);
  g_state.pgxp_gpr[Rd] = g_state.pgxp_gpr[Rs];
}

void CPU::PGXP::CPU_ADDI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs + Imm (signed)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);

  psx_value imm;
  imm.d = instr.i.imm_sext32();

  PGXP_value& prtVal = GetRtValue(instr);
  prtVal = prsVal;

  if (imm.d == 0)
    return;

  if (rsVal == 0)
  {
    // x is low precision value
    prtVal.x = static_cast<float>(imm.sw.l);
    prtVal.y = static_cast<float>(imm.sw.h);
    prtVal.flags |= VALID_X | VALID_Y | VALID_TAINTED_Z;
    prtVal.value = imm.d;
    return;
  }

  prtVal.x = (float)f16Unsign(prtVal.x);
  prtVal.x += (float)imm.w.l;

  // carry on over/underflow
  float of = (prtVal.x > USHRT_MAX) ? 1.f : (prtVal.x < 0) ? -1.f : 0.f;
  prtVal.x = (float)f16Sign(prtVal.x);
  // ret.x -= of * (USHRT_MAX + 1);
  prtVal.y += imm.sw.h + of;

  // truncate on overflow/underflow
  prtVal.y += (prtVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prtVal.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

  prtVal.value = rsVal + imm.d;

  prtVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_ANDI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs & Imm
  const u32 imm = instr.i.imm_zext32();
  const u32 rtVal = rsVal & imm;
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);

  psx_value vRt;
  vRt.d = rtVal;

  PGXP_value& prtVal = GetRtValue(instr);
  prtVal = prsVal;

  prtVal.value = rtVal;
  prtVal.y = 0.f; // remove upper 16-bits
  prtVal.SetValid(COMP_Y);
  prtVal.flags |= VALID_TAINTED_Z;

  switch (imm)
  {
    case 0:
      // if 0 then x == 0
      prtVal.x = 0.0f;
      prtVal.SetValid(COMP_X);
      break;
    case 0xFFFF:
      // if saturated then x == x
      break;
    default:
      // otherwise x is low precision value
      prtVal.x = vRt.sw.l;
      prtVal.SetValid(COMP_X);
      break;
  }
}

void CPU::PGXP::CPU_ORI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs | Imm
  const u32 imm = instr.i.imm_zext32();
  const u32 rtVal = rsVal | imm;

  PGXP_value& pRsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& pRtVal = GetRtValue(instr);
  pRtVal = pRsVal;
  pRtVal.value = rtVal;

  psx_value vRt;
  vRt.d = rtVal;

  switch (imm)
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      pRtVal.x = vRt.sw.l;
      pRtVal.SetValid(COMP_X);
      pRtVal.flags |= VALID_TAINTED_Z;
      break;
  }
}

void CPU::PGXP::CPU_XORI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs ^ Imm
  const u32 imm = instr.i.imm_zext32();
  const u32 rtVal = rsVal ^ imm;

  PGXP_value& pRsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& pRtVal = GetRtValue(instr);
  pRtVal = pRsVal;
  pRtVal.value = rtVal;

  psx_value vRt;
  vRt.d = rtVal;

  switch (imm)
  {
    case 0:
      // if 0 then x == x
      break;
    default:
      // otherwise x is low precision value
      pRtVal.x = vRt.sw.l;
      pRtVal.SetValid(COMP_X);
      pRtVal.flags |= VALID_TAINTED_Z;
      break;
  }
}

void CPU::PGXP::CPU_SLTI(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs < Imm (signed)
  const s32 imm = instr.i.imm_s16();
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);

  const float fimmx = static_cast<float>(imm);
  const float fimmy = fimmx < 0.0f ? -1.0f : 0.0f;

  PGXP_value& prtVal = GetRtValue(instr);
  prtVal.x = (prsVal.GetValidY(rsVal) < fimmy || prsVal.GetValidX(rsVal) < fimmx) ? 1.f : 0.f;
  prtVal.y = 0.0f;
  prtVal.z = prsVal.z;
  prtVal.flags = prsVal.flags | VALID_X | VALID_Y | VALID_TAINTED_Z;
  prtVal.value = BoolToUInt32(static_cast<s32>(rsVal) < imm);
}

void CPU::PGXP::CPU_SLTIU(Instruction instr, u32 rsVal)
{
  LOG_VALUES_C1(instr.i.rs.GetValue(), rsVal);

  // Rt = Rs < Imm (Unsigned)
  const u32 imm = instr.i.imm_u16();
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);

  const float fimmx = static_cast<float>(static_cast<s16>(imm)); // deliberately signed
  const float fimmy = fimmx < 0.0f ? -1.0f : 0.0f;

  PGXP_value& prtVal = GetRtValue(instr);
  prtVal.x =
    (f16Unsign(prsVal.GetValidY(rsVal)) < f16Unsign(fimmy) || f16Unsign(prsVal.GetValidX(rsVal)) < fimmx) ? 1.0f : 0.0f;
  prtVal.y = 0.f;
  prtVal.z = prsVal.z;
  prtVal.flags = prsVal.flags | VALID_X | VALID_Y | VALID_TAINTED_Z;
  prtVal.value = BoolToUInt32(rsVal < imm);
}

////////////////////////////////////
// Load Upper
////////////////////////////////////
void CPU::PGXP::CPU_LUI(Instruction instr)
{
  LOG_VALUES_NV();

  // Rt = Imm << 16
  PGXP_value& pRtVal = GetRtValue(instr);
  pRtVal = PGXP_value_zero;
  pRtVal.y = static_cast<float>(instr.i.imm_s16());
  pRtVal.value = instr.i.imm_zext32() << 16;
  pRtVal.flags = VALID_XY;
}

////////////////////////////////////
// Register Arithmetic
////////////////////////////////////

void CPU::PGXP::CPU_ADD(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs + Rt (signed)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXP_value& prdVal = GetRdValue(instr);

  if (rtVal == 0)
  {
    prdVal = prsVal;
    CopyZIfMissing(prdVal, prtVal);
  }
  else if (rsVal == 0)
  {
    prdVal = prtVal;
    CopyZIfMissing(prdVal, prsVal);
  }
  else
  {
    const double x = f16Unsign(prsVal.GetValidX(rsVal)) + f16Unsign(prtVal.GetValidX(rtVal));

    // carry on over/underflow
    const float of = (x > USHRT_MAX) ? 1.f : (x < 0) ? -1.f : 0.f;
    prdVal.x = static_cast<float>(f16Sign(x));
    // prdVal.x -= of * (USHRT_MAX + 1);
    prdVal.y = prsVal.GetValidY(rsVal) + prtVal.GetValidY(rtVal) + of;

    // truncate on overflow/underflow
    prdVal.y += (prdVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prdVal.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

    prdVal.value = rsVal + rtVal;

    // valid x/y only if one side had a valid x/y
    prdVal.flags = prsVal.flags | (prtVal.flags & VALID_XY) | VALID_TAINTED_Z;

    SelectZ(prdVal, prsVal, prtVal);
  }
}

void CPU::PGXP::CPU_SUB(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs - Rt (signed)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXP_value& prdVal = GetRdValue(instr);

  if (rtVal == 0)
  {
    prdVal = prsVal;
    CopyZIfMissing(prdVal, prtVal);
  }
  else
  {
    const double x = f16Unsign(prsVal.GetValidX(rsVal)) - f16Unsign(prtVal.GetValidX(rtVal));

    // carry on over/underflow
    const float of = (x > USHRT_MAX) ? 1.f : (x < 0) ? -1.f : 0.f;
    prdVal.x = static_cast<float>(f16Sign(x));
    // prdVal.x -= of * (USHRT_MAX + 1);
    prdVal.y = prsVal.GetValidY(rsVal) - (prtVal.GetValidY(rtVal) - of);

    // truncate on overflow/underflow
    prdVal.y += (prdVal.y > SHRT_MAX) ? -(USHRT_MAX + 1) : (prdVal.y < SHRT_MIN) ? USHRT_MAX + 1 : 0.f;

    prdVal.value = rsVal - rtVal;

    // valid x/y only if one side had a valid x/y
    prdVal.flags = prsVal.flags | (prtVal.flags & VALID_XY) | VALID_TAINTED_Z;

    SelectZ(prdVal, prsVal, prtVal);
  }
}

ALWAYS_INLINE_RELEASE void CPU::PGXP::CPU_BITWISE(Instruction instr, u32 rdVal, u32 rsVal, u32 rtVal)
{
  // Rd = Rs & Rt
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  psx_value vald, vals, valt;
  vald.d = rdVal;
  vals.d = rsVal;
  valt.d = rtVal;

  PGXP_value ret;
  ret.flags = ((prsVal.flags | prtVal.flags) & VALID_XY) ? (VALID_XY | VALID_TAINTED_Z) : 0;

  if (vald.w.l == 0)
    ret.x = 0.f;
  else if (vald.w.l == vals.w.l)
    ret.x = prsVal.GetValidX(rsVal);
  else if (vald.w.l == valt.w.l)
    ret.x = prtVal.GetValidX(rtVal);
  else
    ret.x = static_cast<float>(vald.sw.l);

  if (vald.w.h == 0)
    ret.y = 0.f;
  else if (vald.w.h == vals.w.h)
    ret.y = prsVal.GetValidY(rsVal);
  else if (vald.w.h == valt.w.h)
    ret.y = prtVal.GetValidY(rtVal);
  else
    ret.y = static_cast<float>(vald.sw.h);

  SelectZ(ret, prsVal, prtVal);

  ret.value = rdVal;
  GetRdValue(instr) = ret;
}

void CPU::PGXP::CPU_AND_(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs & Rt
  const u32 rdVal = rsVal & rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_OR_(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs | Rt
  const u32 rdVal = rsVal | rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_XOR_(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs ^ Rt
  const u32 rdVal = rsVal ^ rtVal;
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_NOR(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs NOR Rt
  const u32 rdVal = ~(rsVal | rtVal);
  CPU_BITWISE(instr, rdVal, rsVal, rtVal);
}

void CPU::PGXP::CPU_SLT(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs < Rt (signed)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXP_value ret = prsVal;
  ret.x = (prsVal.GetValidY(rsVal) < prtVal.GetValidY(rtVal) ||
           f16Unsign(prsVal.GetValidX(rsVal)) < f16Unsign(prtVal.GetValidX(rtVal))) ?
            1.f :
            0.f;
  ret.y = 0.f;
  ret.flags |= VALID_TAINTED_Z | VALID_X | VALID_Y;
  ret.value = BoolToUInt32(static_cast<s32>(rsVal) < static_cast<s32>(rtVal));
  GetRdValue(instr) = ret;
}

void CPU::PGXP::CPU_SLTU(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Rd = Rs < Rt (unsigned)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXP_value ret = prsVal;
  ret.x = (f16Unsign(prsVal.GetValidY(rsVal)) < f16Unsign(prtVal.GetValidY(rtVal)) ||
           f16Unsign(prsVal.GetValidX(rsVal)) < f16Unsign(prtVal.GetValidX(rtVal))) ?
            1.f :
            0.f;
  ret.y = 0.f;
  ret.flags |= VALID_TAINTED_Z | VALID_X | VALID_Y;
  ret.value = BoolToUInt32(rsVal < rtVal);
  GetRdValue(instr) = ret;
}

////////////////////////////////////
// Register mult/div
////////////////////////////////////

void CPU::PGXP::CPU_MULT(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Hi/Lo = Rs * Rt (signed)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXP_value& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXP_value& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  const float rsx = prsVal.GetValidX(rsVal);
  const float rsy = prsVal.GetValidY(rsVal);
  const float rtx = prtVal.GetValidX(rtVal);
  const float rty = prtVal.GetValidY(rtVal);

  // Multiply out components
  xx = f16Unsign(rsx) * f16Unsign(rtx);
  xy = f16Unsign(rsx) * (rty);
  yx = (rsy)*f16Unsign(rtx);
  yy = (rsy) * (rty);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  ploVal.x = (float)f16Sign(lx);
  ploVal.y = (float)f16Sign(ly);
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);
  phiVal.x = (float)f16Sign(hx);
  phiVal.y = (float)f16Sign(hy);
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  // compute PSX value
  const u64 result = static_cast<u64>(static_cast<s64>(SignExtend64(rsVal)) * static_cast<s64>(SignExtend64(rtVal)));
  phiVal.value = Truncate32(result >> 32);
  ploVal.value = Truncate32(result);
}

void CPU::PGXP::CPU_MULTU(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Hi/Lo = Rs * Rt (unsigned)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXP_value& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXP_value& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  double xx, xy, yx, yy;
  double lx = 0, ly = 0, hx = 0, hy = 0;

  const float rsx = prsVal.GetValidX(rsVal);
  const float rsy = prsVal.GetValidY(rsVal);
  const float rtx = prtVal.GetValidX(rtVal);
  const float rty = prtVal.GetValidY(rtVal);

  // Multiply out components
  xx = f16Unsign(rsx) * f16Unsign(rtx);
  xy = f16Unsign(rsx) * f16Unsign(rty);
  yx = f16Unsign(rsy) * f16Unsign(rtx);
  yy = f16Unsign(rsy) * f16Unsign(rty);

  // Split values into outputs
  lx = xx;

  ly = f16Overflow(xx);
  ly += xy + yx;

  hx = f16Overflow(ly);
  hx += yy;

  hy = f16Overflow(hx);

  ploVal.x = (float)f16Sign(lx);
  ploVal.y = (float)f16Sign(ly);
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);
  phiVal.x = (float)f16Sign(hx);
  phiVal.y = (float)f16Sign(hy);
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  // compute PSX value
  const u64 result = ZeroExtend64(rsVal) * ZeroExtend64(rtVal);
  phiVal.value = Truncate32(result >> 32);
  ploVal.value = Truncate32(result);
}

void CPU::PGXP::CPU_DIV(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Lo = Rs / Rt (signed)
  // Hi = Rs % Rt (signed)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXP_value& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXP_value& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  double vs = f16Unsign(prsVal.GetValidX(rsVal)) + prsVal.GetValidY(rsVal) * (double)(1 << 16);
  double vt = f16Unsign(prtVal.GetValidX(rtVal)) + prtVal.GetValidY(rtVal) * (double)(1 << 16);

  double lo = vs / vt;
  ploVal.y = (float)f16Sign(f16Overflow(lo));
  ploVal.x = (float)f16Sign(lo);
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  double hi = fmod(vs, vt);
  phiVal.y = (float)f16Sign(f16Overflow(hi));
  phiVal.x = (float)f16Sign(hi);
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  // compute PSX value
  if (static_cast<s32>(rtVal) == 0)
  {
    // divide by zero
    ploVal.value = (static_cast<s32>(rsVal) >= 0) ? UINT32_C(0xFFFFFFFF) : UINT32_C(1);
    phiVal.value = static_cast<u32>(static_cast<s32>(rsVal));
  }
  else if (rsVal == UINT32_C(0x80000000) && static_cast<s32>(rtVal) == -1)
  {
    // unrepresentable
    ploVal.value = UINT32_C(0x80000000);
    phiVal.value = 0;
  }
  else
  {
    ploVal.value = static_cast<u32>(static_cast<s32>(rsVal) / static_cast<s32>(rtVal));
    phiVal.value = static_cast<u32>(static_cast<s32>(rsVal) % static_cast<s32>(rtVal));
  }
}

void CPU::PGXP::CPU_DIVU(Instruction instr, u32 rsVal, u32 rtVal)
{
  LOG_VALUES_C2(instr.r.rs.GetValue(), rsVal, instr.r.rt.GetValue(), rtVal);

  // Lo = Rs / Rt (unsigned)
  // Hi = Rs % Rt (unsigned)
  PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  PGXP_value& ploVal = g_state.pgxp_gpr[static_cast<u8>(Reg::lo)];
  PGXP_value& phiVal = g_state.pgxp_gpr[static_cast<u8>(Reg::hi)];
  ploVal = prsVal;
  CopyZIfMissing(ploVal, prsVal);

  // Z/valid is the same
  phiVal = ploVal;

  double vs = f16Unsign(prsVal.GetValidX(rsVal)) + f16Unsign(prsVal.GetValidY(rsVal)) * (double)(1 << 16);
  double vt = f16Unsign(prtVal.GetValidX(rtVal)) + f16Unsign(prtVal.GetValidY(rtVal)) * (double)(1 << 16);

  double lo = vs / vt;
  ploVal.y = (float)f16Sign(f16Overflow(lo));
  ploVal.x = (float)f16Sign(lo);
  ploVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  double hi = fmod(vs, vt);
  phiVal.y = (float)f16Sign(f16Overflow(hi));
  phiVal.x = (float)f16Sign(hi);
  phiVal.flags |= VALID_TAINTED_Z | (prtVal.flags & VALID_XY);

  if (rtVal == 0)
  {
    // divide by zero
    ploVal.value = UINT32_C(0xFFFFFFFF);
    phiVal.value = rsVal;
  }
  else
  {
    ploVal.value = rsVal / rtVal;
    phiVal.value = rsVal % rtVal;
  }
}

////////////////////////////////////
// Shift operations (sa)
////////////////////////////////////
void CPU::PGXP::CPU_SLL(Instruction instr, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // Rd = Rt << Sa
  const u32 sh = instr.r.shamt;
  const u32 rdVal = rtVal << sh;
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  // TODO: Shift flags
  double x = f16Unsign(prtVal.x);
  double y = f16Unsign(prtVal.y);
  if (sh >= 32)
  {
    x = 0.f;
    y = 0.f;
  }
  else if (sh == 16)
  {
    y = f16Sign(x);
    x = 0.f;
  }
  else if (sh >= 16)
  {
    y = x * (1 << (sh - 16));
    y = f16Sign(y);
    x = 0.f;
  }
  else
  {
    x = x * (1 << sh);
    y = y * (1 << sh);
    y += f16Overflow(x);
    x = f16Sign(x);
    y = f16Sign(y);
  }

  PGXP_value& prdVal = GetRdValue(instr);
  prdVal = prtVal;
  prdVal.x = static_cast<float>(x);
  prdVal.y = static_cast<float>(y);
  prdVal.value = rdVal;
  prdVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_SRL(Instruction instr, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // Rd = Rt >> Sa
  const u32 sh = instr.r.shamt;
  const u32 rdVal = rtVal >> sh;
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  double x = prtVal.x;
  double y = f16Unsign(prtVal.y);

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.d = iY.d >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (GetRtValue(instr).x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  PGXP_value& prdVal = GetRdValue(instr);
  prdVal = prtVal;
  prdVal.x = static_cast<float>(x);
  prdVal.y = static_cast<float>(y);
  prdVal.value = rdVal;
  prdVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_SRA(Instruction instr, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // Rd = Rt >> Sa
  const u32 sh = instr.r.shamt;
  const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> sh);
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);

  double x = prtVal.x;
  double y = prtVal.y;

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.sd = iY.sd >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (GetRtValue(instr).x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  x = f16Sign(x);
  y = f16Sign(y);

  PGXP_value& prdVal = GetRdValue(instr);
  prdVal = prtVal;
  prdVal.x = static_cast<float>(x);
  prdVal.y = static_cast<float>(y);
  prdVal.value = rdVal;
  prdVal.flags |= VALID_TAINTED_Z;

  // Use low precision/rounded values when we're not shifting an entire component,
  // and it's not originally from a 3D value. Too many false positives in P2/etc.
  // What we probably should do is not set the valid flag on non-3D values to begin
  // with, only letting them become valid when used in another expression.
  if (!(prdVal.flags & VALID_Z) && sh < 16)
  {
    prdVal.flags = 0;
    MakeValid(&prdVal, rdVal);
  }
}

////////////////////////////////////
// Shift operations variable
////////////////////////////////////
void CPU::PGXP::CPU_SLLV(Instruction instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(instr.r.rt.GetValue(), rtVal, instr.r.rs.GetValue(), rsVal);

  // Rd = Rt << Rs
  const u32 rdVal = rtVal << rsVal;
  const u32 sh = rsVal & 0x1F;
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  [[maybe_unused]] PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal); // TODO: Remove me

  double x = f16Unsign(prtVal.x);
  double y = f16Unsign(prtVal.y);
  if (sh >= 32)
  {
    x = 0.f;
    y = 0.f;
  }
  else if (sh == 16)
  {
    y = f16Sign(x);
    x = 0.f;
  }
  else if (sh >= 16)
  {
    y = x * (1 << (sh - 16));
    y = f16Sign(y);
    x = 0.f;
  }
  else
  {
    x = x * (1 << sh);
    y = y * (1 << sh);
    y += f16Overflow(x);
    x = f16Sign(x);
    y = f16Sign(y);
  }

  PGXP_value& prdVal = GetRdValue(instr);
  prdVal = prtVal;
  prdVal.x = static_cast<float>(x);
  prdVal.y = static_cast<float>(y);
  prdVal.value = rdVal;
  prdVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_SRLV(Instruction instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(instr.r.rt.GetValue(), rtVal, instr.r.rs.GetValue(), rsVal);

  // Rd = Rt >> Sa
  const u32 rdVal = rtVal >> rsVal;
  const u32 sh = rsVal & 0x1F;
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  [[maybe_unused]] PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal); // TODO: remove me

  double x = prtVal.x;
  double y = f16Unsign(prtVal.y);

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.d = iY.d >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (GetRtValue(instr).x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  PGXP_value& prdVal = GetRdValue(instr);
  prdVal = prtVal;
  prdVal.x = static_cast<float>(f16Sign(x));
  prdVal.y = static_cast<float>(f16Sign(y));
  prdVal.value = rdVal;
  prdVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_SRAV(Instruction instr, u32 rtVal, u32 rsVal)
{
  LOG_VALUES_C2(instr.r.rt.GetValue(), rtVal, instr.r.rs.GetValue(), rsVal);

  // Rd = Rt >> Sa
  const u32 rdVal = static_cast<u32>(static_cast<s32>(rtVal) >> rsVal);
  const u32 sh = rsVal & 0x1F;
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  [[maybe_unused]] PGXP_value& prsVal = ValidateAndGetRsValue(instr, rsVal); // TODO: remove me

  double x = prtVal.x;
  double y = prtVal.y;

  psx_value iX;
  iX.d = rtVal;
  psx_value iY;
  iY.d = rtVal;

  iX.sd = (iX.sd << 16) >> 16; // remove Y
  iY.sw.l = iX.sw.h;           // overwrite x with sign(x)

  // Shift test values
  psx_value dX;
  dX.sd = iX.sd >> sh;
  psx_value dY;
  dY.sd = iY.sd >> sh;

  if (dX.sw.l != iX.sw.h)
    x = x / (1 << sh);
  else
    x = dX.sw.l; // only sign bits left

  if (dY.sw.l != iX.sw.h)
  {
    if (sh == 16)
    {
      x = y;
    }
    else if (sh < 16)
    {
      x += y * (1 << (16 - sh));
      if (GetRtValue(instr).x < 0)
        x += 1 << (16 - sh);
    }
    else
    {
      x += y / (1 << (sh - 16));
    }
  }

  if ((dY.sw.h == 0) || (dY.sw.h == -1))
    y = dY.sw.h;
  else
    y = y / (1 << sh);

  PGXP_value& prdVal = GetRdValue(instr);
  prdVal = prtVal;
  prdVal.x = static_cast<float>(f16Sign(x));
  prdVal.y = static_cast<float>(f16Sign(y));
  prdVal.value = rdVal;
  prdVal.flags |= VALID_TAINTED_Z;
}

void CPU::PGXP::CPU_MFC0(Instruction instr, u32 rdVal)
{
  const u32 idx = static_cast<u8>(instr.r.rd.GetValue());
  PGXP_value& prdVal = g_state.pgxp_cop0[idx];

  LOG_VALUES_1(TinyString::from_format("cop0_{}", idx).c_str(), rdVal, &prdVal);

  // CPU[Rt] = CP0[Rd]
  Validate(&prdVal, rdVal);
  PGXP_value& prtVal = GetRtValue(instr);
  prtVal = prdVal;
  prtVal.value = rdVal;
}

void CPU::PGXP::CPU_MTC0(Instruction instr, u32 rdVal, u32 rtVal)
{
  LOG_VALUES_C1(instr.r.rt.GetValue(), rtVal);

  // CP0[Rd] = CPU[Rt]
  PGXP_value& prtVal = ValidateAndGetRtValue(instr, rtVal);
  PGXP_value& prdVal = g_state.pgxp_cop0[static_cast<u8>(instr.r.rd.GetValue())];
  prdVal = prtVal;
  prtVal.value = rdVal;
}
