// include/utils/ErrorContextFormat.h
#ifndef ERROR_CONTEXT_FORMAT_H
#define ERROR_CONTEXT_FORMAT_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

/**
 * @brief Compact JSON for boiler/error/context (header-only, native-testable).
 *
 * Writes directly into the output buffer (no heap, no temporary string buffers) so it fits
 * the 320-byte MQTT publish payload. Short keys:
 *   ec error code, c component, d description, ts uptime ms, task/tp task name/priority,
 *   hf/hm/hb free/minimum free/largest free heap block, ss system state bits,
 *   rq burner request bits, bo/br/wt boiler output/return/tank (tenths °C, null if
 *   invalid), p pressure (hundredths BAR), rd/ra relays desired/actual.
 * The description is shortened when the message would not fit.
 */
namespace ErrorContextFormat {

    struct Fields {
        int32_t errorCode;
        const char* component;
        const char* description;
        uint32_t timestampMs;
        const char* taskName;
        uint8_t taskPriority;
        uint32_t freeHeap;
        uint32_t minFreeHeap;
        uint32_t largestFreeBlock;
        uint32_t systemStateBits;
        uint32_t burnerRequestBits;
        bool sensorsValid;
        int16_t boilerOutput;
        int16_t boilerReturn;
        int16_t waterTank;
        int16_t pressure;
        uint8_t relayDesired;
        uint8_t relayActual;
    };

    namespace detail {
        struct Writer {
            char* p;
            size_t left;  // bytes left including the terminator
            bool ok;
        };

        inline void raw(Writer& w, const char* s) {
            while (*s) {
                if (w.left <= 1) { w.ok = false; return; }
                *w.p++ = *s++;
                w.left--;
            }
        }

        inline void number(Writer& w, const char* fmt, long long v) {
            char num[24];
            snprintf(num, sizeof(num), fmt, v);
            raw(w, num);
        }

        // JSON string body: escapes " and \, drops control characters, at most maxChars source chars
        inline void escaped(Writer& w, const char* s, size_t maxChars) {
            size_t used = 0;
            for (; s && *s && used < maxChars; s++, used++) {
                const unsigned char c = static_cast<unsigned char>(*s);
                if (c < 0x20) continue;
                if (c == '"' || c == '\\') {
                    if (w.left <= 2) { w.ok = false; return; }
                    *w.p++ = '\\';
                    w.left--;
                }
                if (w.left <= 1) { w.ok = false; return; }
                *w.p++ = static_cast<char>(c);
                w.left--;
            }
        }

        inline bool attempt(char* out, size_t size, const Fields& f, size_t descriptionChars) {
            Writer w = {out, size, true};
            raw(w, "{\"ec\":");     number(w, "%lld", f.errorCode);
            raw(w, ",\"c\":\"");    escaped(w, f.component, 32);
            raw(w, "\",\"d\":\"");  escaped(w, f.description, descriptionChars);
            raw(w, "\",\"ts\":");   number(w, "%lld", static_cast<long long>(f.timestampMs));
            raw(w, ",\"task\":\""); escaped(w, f.taskName, 32);
            raw(w, "\",\"tp\":");   number(w, "%lld", f.taskPriority);
            raw(w, ",\"hf\":");     number(w, "%lld", static_cast<long long>(f.freeHeap));
            raw(w, ",\"hm\":");     number(w, "%lld", static_cast<long long>(f.minFreeHeap));
            raw(w, ",\"hb\":");     number(w, "%lld", static_cast<long long>(f.largestFreeBlock));
            raw(w, ",\"ss\":");     number(w, "%lld", static_cast<long long>(f.systemStateBits));
            raw(w, ",\"rq\":");     number(w, "%lld", static_cast<long long>(f.burnerRequestBits));
            if (f.sensorsValid) {
                raw(w, ",\"bo\":"); number(w, "%lld", f.boilerOutput);
                raw(w, ",\"br\":"); number(w, "%lld", f.boilerReturn);
                raw(w, ",\"wt\":"); number(w, "%lld", f.waterTank);
                raw(w, ",\"p\":");  number(w, "%lld", f.pressure);
            } else {
                raw(w, ",\"bo\":null,\"br\":null,\"wt\":null,\"p\":null");
            }
            raw(w, ",\"rd\":");     number(w, "%lld", f.relayDesired);
            raw(w, ",\"ra\":");     number(w, "%lld", f.relayActual);
            raw(w, "}");
            if (w.left == 0) return false;
            *w.p = '\0';
            return w.ok;
        }
    }  // namespace detail

    /**
     * @return length written (excluding terminator), or 0 if even an empty description
     *         does not fit (out is then an empty string)
     */
    inline size_t format(char* out, size_t size, const Fields& f) {
        if (out == nullptr || size == 0) return 0;
        static constexpr size_t DESCRIPTION_LIMITS[] = {256, 40, 16, 0};
        for (size_t limit : DESCRIPTION_LIMITS) {
            if (detail::attempt(out, size, f, limit)) {
                return strlen(out);
            }
        }
        out[0] = '\0';
        return 0;
    }

}  // namespace ErrorContextFormat

#endif  // ERROR_CONTEXT_FORMAT_H
