"""Source contracts for the renamed Windows game-folder loader."""
from pathlib import Path
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding="utf-8")


class ClawshotLoaderTests(unittest.TestCase):
    def test_windows_build_stages_clawshot_and_keeps_game_binary_name(self):
        source = read("nx2/make_game.py")
        self.assertIn('launcher_name = BESIDE[0] if LINUX else "clawshot.exe"', source)
        self.assertIn('os.path.join(game_dir, n)', source)
        self.assertIn('os.path.join(out, launcher_name)', source)
        self.assertIn('legacy_launcher = BESIDE[0] if not LINUX and launcher_name != BESIDE[0] else None', source)
        self.assertIn('os.path.join(out, "bin") if launcher else out', source)

    def test_loader_recognizes_new_and_legacy_names(self):
        source = read("builder/setup/setup.c")
        self.assertIn('g_game_launcher = !_wcsicmp(name, L"clawshot.exe")', source)
        self.assertIn('g_game_launcher || !_wcsicmp(name, L"hookshot.exe")', source)
        self.assertIn('if ((!_wcsicmp(name, L"clawshot.exe") || !_wcsicmp(name, L"hookshot.exe"))', source)
        self.assertIn(r'L"%ls\\bin\\hookshot.exe"', source)

    def test_loader_window_uses_clawshot_brand_only_in_game_folder_mode(self):
        xaml = read("builder/HookshotBuilder/MainWindow.xaml")
        ET.fromstring(xaml)
        code = read("builder/HookshotBuilder/MainWindow.xaml.cs")
        self.assertIn('if (IsClawshotEntry())', code)
        self.assertIn('Title = "Clawshot"', code)
        self.assertIn('SplashBrand.Text = "Clawshot"', code)
        self.assertIn('HeaderBrand.Text = "Clawshot"', code)
        launcher = code[code.index("async void ShowLauncher("):code.index("/// (a folder built before", code.index("async void ShowLauncher("))]
        self.assertIn('"Clawshot"', launcher)
        self.assertNotIn('"Hookshot"', launcher)

    def test_setup_and_update_dialogs_use_clawshot_in_loader_mode(self):
        setup = read("builder/setup/setup.c")
        self.assertIn('setup_title(void) { return g_game_launcher ? L"Clawshot"', setup)
        self.assertIn('status(L"Looking for the latest %ls...", product_name())', setup)
        self.assertIn('status(L"Starting %ls...", product_name())', setup)
        updater = read("builder/HookshotBuilder/Updater.cs")
        self.assertIn('w.ClawshotEntry ? "Downloading the new Clawshot update"', updater)
        self.assertIn('w.ClawshotEntry ? "The Clawshot files were not found', updater)

    def test_shortcuts_prefer_clawshot_and_migrate_old_links(self):
        finder = read("builder/HookshotBuilder/Finder.cs")
        self.assertIn('"Clawshot.lnk"', finder)
        self.assertIn('"Hookshot.lnk"', finder)
        self.assertIn('File.Exists(clawshot) ? clawshot : Path.Combine(gameFolder, "hookshot.exe")', finder)
        self.assertIn('if (File.Exists(oldLink)) File.Delete(oldLink)', finder)

    def test_runtime_update_prefers_clawshot_with_legacy_fallback(self):
        updater = read("host/hos_update.c")
        block = updater[updater.index("static int local_builder("):updater.index("void hos_update_check(void)")]
        self.assertLess(block.index('L"%ls\\\\clawshot.exe"'), block.index('L"%ls\\\\hookshot.exe"'))
        self.assertIn('L"Clawshot update"', updater)
        paths = read("host/hos_paths.h")
        self.assertIn('clawshot.exe in the folder is the launcher', paths)
        self.assertIn(r'folder\clawshot.exe', paths)

    def test_user_docs_name_the_windows_loader_and_keep_linux_name(self):
        docs = read("readme.md")
        self.assertIn('in `clawshot.exe` on Windows', docs)
        self.assertIn('beside `clawshot.exe` on Windows or `hookshot` on Linux', docs)
        self.assertIn('`clawshot.exe` is the launcher; on Linux, `hookshot` is the launcher', docs)


if __name__ == "__main__":
    unittest.main()
