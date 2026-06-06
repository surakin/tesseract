// win32_textserv2_compat.h — RichEdit "2" interface declarations for mingw-w64.
//
// mingw-w64's <textserv.h> ships only the original ITextHost / ITextServices
// interfaces; it has never carried the windowless-RichEdit Direct2D additions
// (ITextHost2, ITextServices2, TxDrawD2D, TXTBIT_D2DDWRITE) that live in the
// proprietary Windows SDK. Win32RichEditArea (host_win32.cpp) needs them to
// route MSFTEDIT's internal rendering through D2D/DirectWrite for colour emoji.
//
// This header supplies just those missing pieces. It is guarded on
// TXTBIT_D2DDWRITE: the real Windows SDK defines that macro, so under MSVC (or
// any future mingw that gains these symbols) the whole file compiles to
// nothing and the genuine SDK declarations are used unchanged.
//
// The two interfaces derive from mingw's existing ITextHost / ITextServices,
// whose vtable layouts already match the canonical SDK interfaces, so the
// combined vtables match exactly what MSFTEDIT.DLL expects. Method order and
// signatures below mirror the Windows SDK <TextServ.h>.

#ifndef TESSERACT_WIN32_TEXTSERV2_COMPAT_H
#define TESSERACT_WIN32_TEXTSERV2_COMPAT_H

// Must be included after <textserv.h> (ITextHost / ITextServices / TXTVIEW)
// and a Direct2D header providing ID2D1RenderTarget (d2d1.h).
#ifndef TXTBIT_D2DDWRITE

// Route this text-services instance through D2D/DirectWrite rather than
// GDI/Uniscribe. Value confirmed against the Windows SDK <TextServ.h>.
#define TXTBIT_D2DDWRITE 0x1000000

// ITextHost2 — extends ITextHost with 12 host-service methods.
struct __declspec(novtable) ITextHost2 : public ITextHost
{
    virtual BOOL     STDMETHODCALLTYPE TxIsDoubleClickPending() = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxGetWindow(HWND* phwnd) = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxSetForegroundWindow() = 0;
    virtual HPALETTE STDMETHODCALLTYPE TxGetPalette() = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxGetEastAsianFlags(LONG* pFlags) = 0;
    virtual HCURSOR  STDMETHODCALLTYPE TxSetCursor2(HCURSOR hcur, BOOL bText) = 0;
    virtual void     STDMETHODCALLTYPE TxFreeTextServicesNotification() = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxGetEditStyle(DWORD dwItem,
                                                      DWORD* pdwData) = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxGetWindowStyles(DWORD* pdwStyle,
                                                         DWORD* pdwExStyle) = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxShowDropCaret(BOOL fShow, HDC hdc,
                                                       LPCRECT prc) = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxDestroyCaret() = 0;
    virtual HRESULT  STDMETHODCALLTYPE TxGetHorzExtent(LONG* plHorzExtent) = 0;
};

// ITextServices2 — extends ITextServices with the D2D draw entry points.
// TxGetNaturalSize2 must precede TxDrawD2D so the latter lands in the correct
// vtable slot (TxDrawD2D is the only method the host actually calls).
struct __declspec(novtable) ITextServices2 : public ITextServices
{
    virtual HRESULT STDMETHODCALLTYPE TxGetNaturalSize2(
        DWORD dwAspect, HDC hdcDraw, HDC hicTargetDev, DVTARGETDEVICE* ptd,
        DWORD dwMode, const SIZEL* psizelExtent, LONG* pwidth, LONG* pheight,
        LONG* pascent) = 0;
    virtual HRESULT STDMETHODCALLTYPE TxDrawD2D(ID2D1RenderTarget* pRenderTarget,
                                                LPCRECTL lprcBounds,
                                                LPRECT lprcUpdate,
                                                LONG lViewId) = 0;
};

#endif // TXTBIT_D2DDWRITE

#endif // TESSERACT_WIN32_TEXTSERV2_COMPAT_H
