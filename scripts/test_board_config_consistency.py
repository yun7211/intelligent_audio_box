import re
import unittest
from pathlib import Path


PROJECT_ROOT = Path(__file__).resolve().parents[1]
KCONFIG_PATH = PROJECT_ROOT / "main" / "Kconfig.projbuild"
CMAKE_PATH = PROJECT_ROOT / "main" / "CMakeLists.txt"
BOARDS_ROOT = PROJECT_ROOT / "main" / "boards"
LOCALES_ROOT = PROJECT_ROOT / "main" / "assets" / "locales"
PARTITIONS_ROOT = PROJECT_ROOT / "partitions"
COMMON_BOARDS_ROOT = BOARDS_ROOT / "common"
COMPONENT_MANIFEST_PATH = PROJECT_ROOT / "main" / "idf_component.yml"

SUPPORTED_BOARDS = {
    "BOARD_TYPE_ESP_BOX_3": "esp-box-3",
    "BOARD_TYPE_LICHUANG_DEV_S3": "lichuang-dev",
}
SUPPORTED_LOCALES = {"zh-CN"}
SUPPORTED_TARGET_DEFAULTS = {"sdkconfig.defaults", "sdkconfig.defaults.esp32s3"}
SUPPORTED_PARTITION_FILES = {"v2/16m.csv", "v2/README.md"}
# Modules that belonged to boards removed in the S3 repository cleanup. The
# eight that still had source files are archived under boards/common/legacy/
# (not deleted); this list enforces that none remain active at the top level
# of boards/common/.
FORBIDDEN_COMMON_MODULES = {
    "adc_battery_monitor",
    "axp2101",
    "dual_network_board",
    "esp_video",
    "ethernet_board",
    "knob",
    "lamp_controller",
    "ml307_board",
    "nt26_board",
    "power_save_timer",
    "press_to_talk_mcp_tool",
    "rndis_board",
    "sleep_timer",
    "sy6970",
    "system_reset",
}


class BoardConfigConsistencyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.kconfig = KCONFIG_PATH.read_text(encoding="utf-8")
        cls.cmake = CMAKE_PATH.read_text(encoding="utf-8")
        cls.component_manifest = COMPONENT_MANIFEST_PATH.read_text(encoding="utf-8")

    def test_kconfig_exposes_only_supported_boards(self):
        configured = set(
            re.findall(
                r"^\s*config\s+((?:ETH_)?BOARD_TYPE_[A-Za-z0-9_]+)",
                self.kconfig,
                re.MULTILINE,
            )
        )
        self.assertEqual(configured, set(SUPPORTED_BOARDS))

    def test_no_stale_board_symbols_are_referenced(self):
        kconfig_references = set(
            re.findall(r"\b((?:ETH_)?BOARD_TYPE_[A-Za-z0-9_]+)\b", self.kconfig)
        )
        cmake_references = set(
            re.findall(
                r"\bCONFIG_((?:ETH_)?BOARD_TYPE_[A-Za-z0-9_]+)\b", self.cmake
            )
        )
        references = kconfig_references | cmake_references
        self.assertEqual(references, set(SUPPORTED_BOARDS))

    def test_supported_boards_have_sources_and_release_config(self):
        for symbol, directory in SUPPORTED_BOARDS.items():
            with self.subTest(symbol=symbol):
                board_dir = BOARDS_ROOT / directory
                self.assertTrue(board_dir.is_dir())
                self.assertTrue((board_dir / "config.json").is_file())
                self.assertTrue(any(board_dir.glob("*.cc")))

    def test_no_language_choice_remains(self):
        configured = set(
            re.findall(r"^\s*config\s+(LANGUAGE_[A-Z_]+)", self.kconfig, re.MULTILINE)
        )
        cmake_references = set(
            re.findall(r"\bCONFIG_(LANGUAGE_[A-Z_]+)\b", self.cmake)
        )
        self.assertEqual(configured, set())
        self.assertEqual(cmake_references, set())

    def test_only_supported_locale_directories_exist(self):
        locales = {path.name for path in LOCALES_ROOT.iterdir() if path.is_dir()}
        self.assertEqual(locales, SUPPORTED_LOCALES)

    def test_only_esp32s3_target_defaults_exist(self):
        defaults = {path.name for path in PROJECT_ROOT.glob("sdkconfig.defaults*")}
        self.assertEqual(defaults, SUPPORTED_TARGET_DEFAULTS)

    def test_only_supported_partition_files_exist(self):
        partitions = {
            path.relative_to(PARTITIONS_ROOT).as_posix()
            for path in PARTITIONS_ROOT.rglob("*")
            if path.is_file()
        }
        self.assertEqual(partitions, SUPPORTED_PARTITION_FILES)

    def test_forbidden_board_common_modules_are_archived(self):
        # Archived modules live in boards/common/legacy/, so none of their stems
        # should appear directly under boards/common/ (the actively-compiled set).
        remaining = {
            path.stem
            for path in COMMON_BOARDS_ROOT.iterdir()
            if path.is_file() and path.stem in FORBIDDEN_COMMON_MODULES
        }
        self.assertEqual(remaining, set())

    def test_non_esp32s3_target_branches_are_removed(self):
        target_references = set(
            re.findall(r"\b(?:CONFIG_)?IDF_TARGET_(ESP32[A-Z0-9_]*)\b", self.kconfig + self.cmake)
        )
        self.assertEqual(target_references, {"ESP32S3"})

    def test_acoustic_provisioning_compiles_its_implementation(self):
        acoustic_sources = re.search(
            r"if\s*\(\s*CONFIG_USE_ACOUSTIC_WIFI_PROVISIONING\s*\)"
            r".*?boards/common/afsk_demod\.cc.*?endif\s*\(\s*\)",
            self.cmake,
            re.DOTALL,
        )
        self.assertIsNotNone(acoustic_sources)

    def test_board_validation_allows_idf_early_expansion(self):
        self.assertRegex(
            self.cmake,
            r"else\s*\(\s*\)\s*if\s*\(\s*NOT CMAKE_BUILD_EARLY_EXPANSION\s*\)"
            r"\s*message\s*\(\s*FATAL_ERROR \"No supported board type selected\"\s*\)"
            r"\s*endif\s*\(\s*\)\s*endif\s*\(\s*\)",
        )
        self.assertRegex(
            self.cmake,
            r"if\s*\(\s*NOT BOARD_SOURCES AND NOT CMAKE_BUILD_EARLY_EXPANSION\s*\)"
            r"\s*message\s*\(\s*FATAL_ERROR \"No board sources found for \$\{BOARD_TYPE\}\"\s*\)"
            r"\s*endif\s*\(\s*\)",
        )

    def test_all_explicit_cmake_sources_exist(self):
        sources = set(re.findall(r'"([^"$]+\.(?:c|cc|cpp))"', self.cmake))
        missing = sorted(source for source in sources if not (PROJECT_ROOT / "main" / source).is_file())
        self.assertEqual(missing, [])


if __name__ == "__main__":
    unittest.main()
