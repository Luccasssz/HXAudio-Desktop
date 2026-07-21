; HXAudio Windows Shared Audio Engine AudioLimiter bypass opcodes (x64).
;
; EngineBypass.cpp activates the real Windows AudioLimiter COM class, resolves
; AudioLimiter::APOProcess from its vtable, follows the in-module calls to the
; limiter worker and validates the worker's fast-copy/slow-processing control
; flow.  The four-byte replacement is assembled from these opcodes:
;
;     original: 7A xx        jp   slow_limiter
;               75 yy        jne  slow_limiter
;
;     bypass:   EB zz        jmp  fast_copy
;               90 90        nop / nop
;
; The short-jump displacement is calculated from the validated live Windows
; image, so no Windows build number, audioeng.dll RVA or OEM name is fixed in
; the patch.  The fast-copy path is Microsoft's own unity-gain path; stack,
; buffer metadata and sample count handling remain in the original function.

PUBLIC HXAudioLimiterUnorderedBranchOpcode
PUBLIC HXAudioLimiterUnequalBranchOpcode
PUBLIC HXAudioLimiterJumpOpcode
PUBLIC HXAudioLimiterNopOpcode

.const
HXAudioLimiterUnorderedBranchOpcode BYTE 07Ah
HXAudioLimiterUnequalBranchOpcode   BYTE 075h
HXAudioLimiterJumpOpcode            BYTE 0EBh
HXAudioLimiterNopOpcode             BYTE 090h

END
