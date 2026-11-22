// Production Logitech HID++ poller against a simulated Logi Bolt receiver (046D:C548) with an MX Master 3S
// paired on slot 2. Checks the reading itself and how many requests reach the mouse over the air: device
// details (name, type, unit id, battery feature index) are read once and cached, so later polls send only
// a ping and one battery call.
#include "../src/drivers/drivers.h"

#include <cstdio>
#include <deque>

void PollLogitech(PollContext& ctx);

namespace {
int failures = 0;
const char* current = "";
void Check(bool result, const char* expression, int line) {
    if (!result) { std::printf("FAIL %s:%d %s\n", current, line, expression); ++failures; }
}
#define CHECK(x) Check((x), #x, __LINE__)

constexpr BYTE kSlot = 2;
constexpr BYTE kSwId = 0x0D;
const char kName[] = "MX Master 3S";

// The mouse's feature table: feature id -> index.
struct Feature { USHORT id; BYTE index; };
std::vector<Feature> features = { { 0x0003, 2 }, { 0x0005, 3 }, { 0x1004, 8 } };
BYTE soc = 72, chargeStatus = 0;
bool mouseOnline = true;
int mouseRequests = 0;  // requests sent to the mouse's slot (over the air)
std::deque<std::vector<BYTE>> queue;

BYTE IndexOf(USHORT id) {
    for (auto& f : features) if (f.id == id) return f.index;
    return 0;
}

std::vector<BYTE> Reply(const std::vector<BYTE>& req, std::initializer_list<BYTE> params) {
    std::vector<BYTE> r(20, 0);
    r[0] = 0x11;
    r[1] = req[1];
    r[2] = req[2];
    r[3] = req[3];
    size_t i = 4;
    for (BYTE v : params) r[i++] = v;
    return r;
}

// What the receiver (or the mouse through it) answers to one request.
void Answer(const std::vector<BYTE>& q) {
    BYTE idx = q[1], feat = q[2], fn = q[3] >> 4;
    if (idx != kSlot || !mouseOnline) {  // empty or sleeping slot: the receiver answers "unknown device" itself
        queue.push_back({ 0x10, idx, 0x8F, feat, q[3], 0x09, 0 });
        return;
    }
    ++mouseRequests;
    if (feat == 0x00 && fn == 1) { queue.push_back(Reply(q, { 4, 5, q[6] })); return; }  // ping: HID++ 4.5
    if (feat == 0x00 && fn == 0) { queue.push_back(Reply(q, { IndexOf((USHORT)((q[4] << 8) | q[5])) })); return; }
    if (feat == IndexOf(0x0005)) {
        if (fn == 0) { queue.push_back(Reply(q, { (BYTE)(sizeof(kName) - 1) })); return; }
        if (fn == 1) {
            std::vector<BYTE> r = Reply(q, {});
            for (size_t i = q[4], o = 4; i < sizeof(kName) - 1 && o < 20; ++i, ++o) r[o] = (BYTE)kName[i];
            queue.push_back(r);
            return;
        }
        if (fn == 2) { queue.push_back(Reply(q, { 0x03 })); return; }  // mouse
    }
    if (feat == IndexOf(0x0003) && fn == 0) { queue.push_back(Reply(q, { 2, 0xAB, 0xCD, 0x12, 0x34 })); return; }
    if (feat == IndexOf(0x1004) && fn == 1) { queue.push_back(Reply(q, { soc, 0x04, chargeStatus })); return; }
    queue.push_back({ 0x11, idx, 0xFF, feat, q[3], 0x02 });  // invalid function
}

HidColl Collection(USHORT usage, USHORT outLen, const wchar_t* col) {
    HidColl c;
    c.vid = 0x046D;
    c.pid = 0xC548;
    c.iface = 2;
    c.path = std::wstring(L"\\\\?\\hid#vid_046d&pid_c548&mi_02&") + col + L"#8&2a2b3c4d&0&000" + col[4] + L"#{4d1e55b2}";
    c.caps.UsagePage = 0xFF00;
    c.caps.Usage = usage;
    c.caps.OutputReportByteLength = c.caps.InputReportByteLength = outLen;
    return c;
}

std::vector<DeviceStatus> Poll() {
    PollContext ctx;
    ctx.colls = { Collection(0x0001, 7, L"col01"), Collection(0x0002, 20, L"col02") };
    mouseRequests = 0;
    PollLogitech(ctx);
    return ctx.out;
}
}  // namespace

bool HidDev::Open(const HidColl& c) { c_ = c; fd_ = 1; return true; }
void HidDev::Close() { fd_ = -1; }
void HidDev::Flush() {}
bool HidDev::Write(const std::vector<BYTE>& data, int) {
    Answer(data);
    return true;
}
int HidDev::Read(std::vector<BYTE>& buf, int) {
    if (queue.empty()) return -1;
    buf = queue.front();
    queue.pop_front();
    return (int)buf.size();
}
std::wstring HidProduct(const HidColl&) { return L"USB Receiver"; }
std::wstring Hex4(USHORT v) {
    wchar_t b[8];
    swprintf(b, 8, L"%04X", v);
    return b;
}
int Interpolate(int, const int*, const int*, int) { return -1; }
DeviceKind GuessKind(const std::wstring& name) {
    return name.find(L"Master") != std::wstring::npos ? DeviceKind::Mouse : DeviceKind::Other;
}

int main() {
    current = "first poll reads the MX Master 3S in full";
    auto out = Poll();
    CHECK(out.size() == 1);
    int firstRequests = mouseRequests;
    if (out.size() == 1) {
        CHECK(out[0].name == L"MX Master 3S");
        CHECK(out[0].kind == DeviceKind::Mouse);
        CHECK(out[0].percent == 72 && !out[0].charging);
        CHECK(out[0].key == L"logi:unit:ABCD1234");
    }
    CHECK(firstRequests >= 8);

    current = "later polls use the cached details: a ping and one battery call";
    soc = 71;
    out = Poll();
    CHECK(out.size() == 1);
    CHECK(mouseRequests == 2);
    if (out.size() == 1) {
        CHECK(out[0].name == L"MX Master 3S");
        CHECK(out[0].kind == DeviceKind::Mouse);
        CHECK(out[0].percent == 71);
        CHECK(out[0].key == L"logi:unit:ABCD1234");
    }

    current = "charging state still comes through the cached path";
    chargeStatus = 1;
    out = Poll();
    CHECK(out.size() == 1 && mouseRequests == 2);
    if (out.size() == 1) CHECK(out[0].charging && !out[0].full);
    chargeStatus = 0;

    current = "sleeping mouse: not listed, cache kept";
    mouseOnline = false;
    out = Poll();
    CHECK(out.empty() && mouseRequests == 0);
    mouseOnline = true;
    out = Poll();
    CHECK(out.size() == 1 && mouseRequests == 2);

    current = "feature table changed (firmware update): cached call fails, details read again";
    features = { { 0x0003, 2 }, { 0x0005, 4 }, { 0x1004, 9 } };
    out = Poll();
    CHECK(out.size() == 1 && mouseRequests > 2);
    if (out.size() == 1) CHECK(out[0].percent == 71 && out[0].name == L"MX Master 3S");
    out = Poll();
    CHECK(out.size() == 1 && mouseRequests == 2);

    current = "reset forgets the cache";
    ResetLogitechInfoCache();
    out = Poll();
    CHECK(out.size() == 1 && mouseRequests == firstRequests);

    if (failures) return 1;
    std::printf("Logitech HID++ driver tests passed (Bolt MX Master 3S, cached details, charging, sleep, stale cache, reset).\n");
    return 0;
}
