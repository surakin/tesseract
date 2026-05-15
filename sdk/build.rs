fn main() {
    let platform = match std::env::var("TESSERACT_UI").as_deref() {
        Ok("win32") => "Win32",
        Ok("macos") => "macOS",
        Ok("qt6")   => "Qt6",
        Ok("gtk")   => "GTK4",
        _           => "Desktop",
    };
    println!("cargo:rustc-env=TESSERACT_UI_PLATFORM={platform}");
    println!("cargo:rerun-if-env-changed=TESSERACT_UI");
}
