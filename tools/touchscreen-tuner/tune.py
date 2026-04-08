#!/usr/bin/env python3
"""EGoTouchRev-Linux 触屏算法调节 GUI"""

from gi.repository import Adw, GLib, Gtk, Pango
import glob
import os
import pwd
import subprocess
import sys
import tempfile
import uuid
from pathlib import Path

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")


WRITE_ENABLED = "--write-enabled" in sys.argv


def _arg_value(name):
    for idx, arg in enumerate(sys.argv):
        if arg == name and idx + 1 < len(sys.argv):
            return sys.argv[idx + 1]
        if arg.startswith(f"{name}="):
            return arg.split("=", 1)[1]
    return None


WRITE_READY_FILE = _arg_value("--ready-file")


# ── 本地化 ────────────────────────────────────────────────────────────────────
_TR = {
    "app_title":             "EGoTouchRev-Linux Algo Tuner",
    "mode_label":            "使用模式",
    "mode_daily":            "日用模式",
    "mode_game":             "游戏模式",
    "tab_preprocessing":     "预处理",
    "tab_detection":         "检测",
    "tab_edge":              "边缘补偿",
    "tab_tracking":          "追踪",
    "tab_about":             "关于",
    "btn_refresh":           "重新读取设备参数",
    "btn_reset_defaults":    "恢复默认配置",
    "btn_enable_edit":       "解锁配置修改",
    "btn_close":             "关闭",
    "btn_apply":             "应用",
    "btn_open_link":         "打开链接",
    "status_path":           "当前 sysfs 路径",
    "hint_game":             "游戏模式：已关闭轨迹平滑 / 已启用跳点检测",
    "hint_daily":            "日用模式：已恢复默认轨迹参数",
    "hint_reset_done":       "已恢复默认配置",
    "err_no_algo":           (
        "未找到驱动 sysfs 节点\n\n"
        "请确认 himax-spi 模块已加载。\n"
        "驱动版本可能不匹配。"
    ),
    "err_perm":              "写入 {name} 失败：权限不足，请以 root 运行",
    "err_generic":           "写入 {name} 失败：{err}",
    "err_invalid":           "无效值：「{val}」不是整数",
    "err_open_link":         "打开链接失败：{err}",
    "err_helper_missing":    "未找到提权写入 helper：{path}",
    "cmf_enabled":           "CMF 开关",
    "cmf_exclusion":         "CMF 排除阈值",
    "cmf_max_correction":    "CMF 最大修正量",
    "iir_enabled":           "IIR 开关",
    "iir_decay_weight":      "IIR 衰减权重",
    "iir_decay_step":        "IIR 衰减步长",
    "iir_noise_floor":       "IIR 噪声底",
    "iir_gate_floor":        "IIR 门限底",
    "iir_gate_ratio_q8":     "IIR 门限比率 (Q8)",
    "macro_threshold":       "宏区阈值",
    "peak_threshold":        "峰值阈值",
    "palm_enabled":          "手掌拒绝",
    "palm_area_threshold":   "手掌面积阈值",
    "palm_signal_threshold": "手掌信号阈值",
    "palm_density_low":      "手掌低密度阈值",
    "edge_comp_enabled":     "边缘补偿开关",
    "edge_boost_pct":        "边缘信号增强 (%)",
    "edge_push_q8":          "边缘外推量 (Q8.8)",
    "edge_blend_q8":         "边缘混合范围 (Q8.8)",
    "track_dist2_max":       "最大匹配距离²",
    "track_lost_frames":     "允许丢失帧数",
    "debounce_base":         "防抖基准帧",
    "track_smoothing":       "轨迹平滑",
    "track_active_guard":    "活动保护",
    "track_start_debounce":  "起始防抖帧",
    "track_jump_dist2":      "跳点检测距离²",
}

_DESC = {
    "cmf_enabled":           "消除帧间共模噪声，改善基线稳定性",
    "cmf_exclusion":         "信号高于此值的像素不计入 CMF 均值，避免触点污染基线",
    "cmf_max_correction":    "每行/列 CMF 修正量的上限，防止过度补偿",
    "iir_enabled":           "IIR 时域滤波，平滑帧间信号抖动",
    "iir_decay_weight":      "与历史帧的混合权重（0 = 全历史，256 = 不混合）",
    "iir_decay_step":        "每帧信号的自然衰减量，抬手后加速清零",
    "iir_noise_floor":       "低于此值的信号直接清零，抑制静态噪声",
    "iir_gate_floor":        "动态门限的最低值，防止阈值过低",
    "iir_gate_ratio_q8":     "动态门限 = 帧最大值 × ratio / 256",
    "macro_threshold":       "BFS 种子像素的最低信号值，越高越难触发",
    "peak_threshold":        "峰值候选的最低信号，过滤微弱触点",
    "palm_enabled":          "过滤面积或密度异常的大面积接触",
    "palm_area_threshold":   "像素数 ≥ 此值则判定为手掌",
    "palm_signal_threshold": "信号总和 ≥ 此值则判定为手掌",
    "palm_density_low":      "信号/面积 < 此值则判定为低密度手掌",
    "edge_comp_enabled":     "对边缘像素做信号增强与坐标外推，改善边缘触控精度",
    "edge_boost_pct":        "边缘像素信号放大百分比（50 = 放大 1.5×）",
    "edge_push_q8":          "质心向外最大推移量，Q8.8 定点数（128 = 0.5 格）",
    "edge_blend_q8":         "坐标混合过渡区宽度，Q8.8 定点数（512 = 2 格）",
    "track_dist2_max":       "帧间轨迹匹配的最大欧氏距离²，超出则新建轨迹",
    "track_lost_frames":     "连续丢失多少帧后释放轨迹槽",
    "debounce_base":         "新触点正式上报前须连续稳定的帧数",
    "track_smoothing":       "基于速度预测的坐标平滑，降低抖动（游戏模式建议关闭）",
    "track_active_guard":    "首次稳定触点确认前清除游离轨迹，防误触",
    "track_start_debounce":  "touch_active 确认所需帧数（0 = 立即，游戏模式建议为 0）",
    "track_jump_dist2":      "坐标突变距离²，超出则强制抬起再按下（0 = 禁用）",
}


def t(key, **kwargs):
    s = _TR.get(key, key)
    return s.format(**kwargs) if kwargs else s


def desc(name):
    return _DESC.get(name, "")


DEFAULTS = {
    "cmf_enabled":           1,
    "cmf_exclusion":         250,
    "cmf_max_correction":    500,
    "iir_enabled":           1,
    "iir_decay_weight":      200,
    "iir_decay_step":        80,
    "iir_noise_floor":       5,
    "iir_gate_floor":        200,
    "iir_gate_ratio_q8":     26,
    "macro_threshold":       800,
    "peak_threshold":        800,
    "palm_enabled":          1,
    "palm_area_threshold":   50,
    "palm_signal_threshold": 80000,
    "palm_density_low":      400,
    "edge_comp_enabled":     1,
    "edge_boost_pct":        50,
    "edge_push_q8":          128,
    "edge_blend_q8":         512,
    "track_dist2_max":       176400,
    "track_lost_frames":     3,
    "debounce_base":         2,
    "track_smoothing":       1,
    "track_active_guard":    1,
    "track_start_debounce":  2,
    "track_jump_dist2":      0,
}

GAME_PRESET = {
    "track_smoothing":      0,
    "track_start_debounce": 0,
    "track_jump_dist2":     6400,
}
DAILY_PRESET = {k: DEFAULTS[k] for k in GAME_PRESET}


_KNOWN_PATH = Path(
    "/sys/devices/platform/soc@0/9c0000.geniqup/998000.spi"
    "/spi_master/spi0/spi0.0/algo"
)


def find_algo_dir():
    if _KNOWN_PATH.is_dir():
        return _KNOWN_PATH
    hits = glob.glob("/sys/devices/**/algo/peak_threshold", recursive=True)
    if hits:
        return Path(hits[0]).parent
    return None


ALGO = None


def read_param(name):
    try:
        return int((ALGO / name).read_text().strip())
    except Exception:
        return DEFAULTS.get(name, 0)


def write_param(name, value):
    (ALGO / name).write_text(str(value))


def _desktop_user():
    for key in ("SUDO_USER", "PKEXEC_UID"):
        value = os.environ.get(key)
        if not value:
            continue
        if key == "PKEXEC_UID":
            try:
                return pwd.getpwuid(int(value)).pw_name
            except Exception:
                continue
        return value

    try:
        out = subprocess.check_output(
            ["loginctl", "list-sessions", "--no-legend"],
            text=True,
            stderr=subprocess.DEVNULL,
        )
        for line in out.splitlines():
            parts = line.split()
            if len(parts) < 4:
                continue
            if parts[2] == "root":
                continue
            session_id = parts[0]
            user = parts[2]
            try:
                state = subprocess.check_output(
                    ["loginctl", "show-session", session_id,
                        "-p", "Active", "-p", "State"],
                    text=True,
                    stderr=subprocess.DEVNULL,
                )
            except Exception:
                continue
            if "Active=yes" in state or "State=active" in state:
                return user
    except Exception:
        pass

    try:
        return pwd.getpwuid(os.getuid()).pw_name
    except Exception:
        return None


def open_url(url):
    user = _desktop_user()
    env = {}
    for key in (
        "DISPLAY",
        "WAYLAND_DISPLAY",
        "XAUTHORITY",
        "XDG_SESSION_TYPE",
        "DESKTOP_SESSION",
        "XDG_CURRENT_DESKTOP",
    ):
        value = os.environ.get(key)
        if value:
            env[key] = value

    preexec_fn = None
    if os.getuid() == 0 and user and user != "root":
        pw = pwd.getpwnam(user)
        env["HOME"] = pw.pw_dir
        env["USER"] = user
        env["LOGNAME"] = user
        env["SHELL"] = pw.pw_shell
        env["XDG_RUNTIME_DIR"] = f"/run/user/{pw.pw_uid}"
        env["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path=/run/user/{pw.pw_uid}/bus"

        def preexec_fn():
            os.setgid(pw.pw_gid)
            os.setuid(pw.pw_uid)
    else:
        for key in ("HOME", "USER", "LOGNAME", "SHELL", "XDG_RUNTIME_DIR", "DBUS_SESSION_BUS_ADDRESS"):
            value = os.environ.get(key)
            if value:
                env[key] = value

    subprocess.Popen(
        ["xdg-open", url],
        env=env,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        start_new_session=True,
        preexec_fn=preexec_fn,
    )


TABS = [
    ("tab_preprocessing", [
        ("toggle", "cmf_enabled"),
        ("slider", "cmf_exclusion",          50,   800,  10),
        ("slider", "cmf_max_correction",     50,  1500,  10),
        ("toggle", "iir_enabled"),
        ("slider", "iir_decay_weight",        0,   256,   1),
        ("slider", "iir_decay_step",          0,   200,   1),
        ("entry",  "iir_noise_floor"),
        ("slider", "iir_gate_floor",         20,   500,   5),
        ("entry",  "iir_gate_ratio_q8"),
    ]),
    ("tab_detection", [
        ("slider", "macro_threshold",       100,  2000,  10),
        ("slider", "peak_threshold",        100,  2000,  10),
        ("toggle", "palm_enabled"),
        ("slider", "palm_area_threshold",     5,   150,   1),
        ("entry",  "palm_signal_threshold"),
        ("slider", "palm_density_low",       50,   800,  10),
    ]),
    ("tab_edge", [
        ("toggle", "edge_comp_enabled"),
        ("slider", "edge_boost_pct",          0,   200,   5),
        ("slider", "edge_push_q8",            0,   384,   8),
        ("slider", "edge_blend_q8",          64,  1024,  16),
    ]),
    ("tab_tracking", [
        ("toggle", "track_smoothing"),
        ("toggle", "track_active_guard"),
        ("slider", "track_lost_frames",       1,    10,   1),
        ("slider", "debounce_base",           0,     8,   1),
        ("slider", "track_start_debounce",    0,     8,   1),
        ("entry",  "track_dist2_max"),
        ("entry",  "track_jump_dist2"),
    ]),
    ("tab_about", []),
]

ABOUT_LINKS = [
    (
        "重构驱动",
        "chiyuki0325/EGoTouchRev-Linux",
        "https://github.com/chiyuki0325/EGoTouchRev-Linux",
        "将触摸驱动与算法体系系统化整理，并重构适配到 Linux 内核驱动形态，补齐参数导出与调参能力。",
    ),
    (
        "算法参考",
        "awarson2233/EGoTouchRev",
        "https://github.com/awarson2233/EGoTouchRev",
        "Windows 用户态触控服务，提供 CMF、GridIIR、宏区域检测、掌压抑制、峰值检测、加权质心、触点跟踪等完整算法参考。",
    ),
    (
        "原始驱动",
        "TheUnknownThing/linux-gaokun",
        "https://github.com/TheUnknownThing/linux-gaokun",
        "Linux SPI 多点触控驱动基础，提供 SPI 通信、固件初始化、电源管理、panel follower 等底层实现。",
    ),
    (
        "基础实现",
        "right-0903/linux-gaokun",
        "https://github.com/right-0903/linux-gaokun",
        "最早面向华为 Gaokun 平台打通的 Linux 触控基础实现之一，完成设备 bring-up、初始驱动接线与整机功能验证。",
    ),
]


class TunerWindow(Gtk.Window):
    def __init__(self):
        super().__init__()
        self.set_title(t("app_title"))
        self.set_default_size(720, 820)
        self.set_size_request(500, 360)

        self._row_setters = {}
        self._row_widgets = {}
        self._mode_buttons = {}
        self._updating = False
        self._mode_hint = None

        self._overlay = Adw.ToastOverlay()
        self._root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=0)
        self._overlay.set_child(self._root)
        self.set_child(self._overlay)

        self.reload_ui()

    def _set_body(self, widget):
        if hasattr(self, "_body") and self._body is not None:
            self._root.remove(self._body)
        self._body = widget
        self._root.append(widget)

    def show_toast(self, text, timeout=3):
        toast = Adw.Toast.new(text)
        toast.set_timeout(timeout)
        self._overlay.add_toast(toast)

    def reload_ui(self):
        global ALGO
        ALGO = find_algo_dir()
        self._row_setters = {}
        self._row_widgets = {}
        self._mode_buttons = {}
        if ALGO is None:
            self._set_body(self._build_fatal_page())
        else:
            self._set_body(self._build_main_ui())

    def _make_card(self, margin_top=0, margin_bottom=0):
        frame = Gtk.Frame(margin_top=margin_top, margin_bottom=margin_bottom)
        frame.add_css_class("card")
        return frame

    def _make_card_box(self, spacing=6, margins=12):
        return Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=spacing,
            margin_start=margins,
            margin_end=margins,
            margin_top=10,
            margin_bottom=10,
        )

    def _build_fatal_page(self):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL)
        box.set_vexpand(True)
        box.set_hexpand(True)

        page = Adw.StatusPage()
        page.set_icon_name("dialog-warning-symbolic")
        page.set_title(t("app_title"))
        page.set_description(t("err_no_algo"))
        page.set_vexpand(True)

        actions = Gtk.Box(
            orientation=Gtk.Orientation.HORIZONTAL,
            spacing=12,
            halign=Gtk.Align.CENTER,
            margin_top=12,
        )
        retry_btn = Gtk.Button(label=t("btn_refresh"))
        retry_btn.connect("clicked", lambda *_: self.reload_ui())
        actions.append(retry_btn)

        page.set_child(actions)
        box.append(page)
        return box

    def _build_main_ui(self):
        outer = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=0,
            margin_top=8,
            margin_bottom=8,
        )
        outer.set_vexpand(True)

        clamp = Adw.Clamp(maximum_size=760, tightening_threshold=420)
        content = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=10,
            margin_start=8,
            margin_end=8,
            margin_top=4,
            margin_bottom=4,
        )
        clamp.set_child(content)
        outer.append(clamp)

        content.append(self._build_path_card())
        content.append(self._build_mode_card())

        switcher = Gtk.StackSwitcher(
            halign=Gtk.Align.CENTER,
            margin_top=2,
            margin_bottom=2,
        )
        stack = Gtk.Stack(
            vexpand=True,
            hexpand=True,
            transition_type=Gtk.StackTransitionType.SLIDE_LEFT_RIGHT,
        )
        switcher.set_stack(stack)
        content.append(switcher)
        content.append(stack)
        content.append(self._build_action_row())

        for tab_key, controls in TABS:
            stack.add_titled(self._build_tab_page(
                controls), tab_key, t(tab_key))

        return outer

    def _build_path_card(self):
        frame = self._make_card()
        row = Gtk.Box(
            orientation=Gtk.Orientation.HORIZONTAL,
            spacing=10,
            margin_start=12,
            margin_end=12,
            margin_top=10,
            margin_bottom=10,
        )
        frame.set_child(row)

        path = Gtk.Label(
            label=str(ALGO),
            xalign=0,
            hexpand=True,
        )
        path.add_css_class("caption")
        path.add_css_class("monospace")
        path.set_ellipsize(Pango.EllipsizeMode.MIDDLE)
        path.set_single_line_mode(True)
        path.set_tooltip_text(str(ALGO))
        row.append(path)
        return frame

    def _build_mode_card(self):
        current_mode = self._detect_mode()
        frame = self._make_card()
        box = self._make_card_box()
        frame.set_child(box)

        title = Gtk.Label(label=t("mode_label"), xalign=0)
        title.add_css_class("heading")
        box.append(title)

        buttons = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
        daily = Gtk.CheckButton(label=t("mode_daily"))
        game = Gtk.CheckButton(label=t("mode_game"))
        game.set_group(daily)
        self._mode_buttons = {"daily": daily, "game": game}
        self._updating = True
        self._mode_buttons[current_mode].set_active(True)
        self._updating = False
        daily.connect("toggled", lambda btn: btn.get_active()
                      and self._on_mode("daily"))
        game.connect("toggled", lambda btn: btn.get_active()
                     and self._on_mode("game"))
        buttons.append(daily)
        buttons.append(game)
        box.append(buttons)

        self._mode_hint = Gtk.Label(
            xalign=0,
            wrap=True,
        )
        self._mode_hint.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
        self._mode_hint.add_css_class("caption")
        self._set_mode_hint(current_mode)
        box.append(self._mode_hint)

        if not WRITE_ENABLED:
            for btn in self._mode_buttons.values():
                btn.set_sensitive(False)
        return frame

    def _build_action_row(self):
        row = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=6)
        buttons = Gtk.Box(
            orientation=Gtk.Orientation.HORIZONTAL,
            spacing=8,
            halign=Gtk.Align.END,
        )
        if not WRITE_ENABLED:
            enable_btn = Gtk.Button(label=t("btn_enable_edit"))
            enable_btn.connect(
                "clicked", lambda *_: self._request_write_mode())
            buttons.append(enable_btn)
        reset_btn = Gtk.Button(label=t("btn_reset_defaults"))
        reset_btn.connect("clicked", lambda *_: self._reset_defaults())
        reset_btn.set_sensitive(WRITE_ENABLED)
        refresh_btn = Gtk.Button(label=t("btn_refresh"))
        refresh_btn.connect("clicked", lambda *_: self.reload_ui())
        buttons.append(reset_btn)
        buttons.append(refresh_btn)
        row.append(buttons)
        return row

    def _build_tab_page(self, controls):
        if not controls:
            return self._build_about_page()

        scroll = Gtk.ScrolledWindow(
            hscrollbar_policy=Gtk.PolicyType.NEVER,
            vscrollbar_policy=Gtk.PolicyType.AUTOMATIC,
            vexpand=True,
        )
        shell = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            margin_start=4,
            margin_end=4,
            margin_top=2,
            margin_bottom=8,
        )
        frame = self._make_card()
        shell.append(frame)

        listbox = Gtk.ListBox(
            selection_mode=Gtk.SelectionMode.NONE,
        )
        listbox.add_css_class("boxed-list")
        frame.set_child(listbox)
        scroll.set_child(shell)

        for ctrl in controls:
            listbox.append(self._build_control(ctrl))

        return scroll

    def _build_about_page(self):
        scroll = Gtk.ScrolledWindow(
            hscrollbar_policy=Gtk.PolicyType.NEVER,
            vscrollbar_policy=Gtk.PolicyType.AUTOMATIC,
            vexpand=True,
        )
        shell = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            margin_start=4,
            margin_end=4,
            margin_top=2,
            margin_bottom=8,
        )
        frame = self._make_card()
        shell.append(frame)
        scroll.set_child(shell)

        box = self._make_card_box(spacing=10)
        frame.set_child(box)

        for idx, (role, name, url, summary) in enumerate(ABOUT_LINKS):
            if idx:
                box.append(Gtk.Separator(
                    orientation=Gtk.Orientation.HORIZONTAL))

            row = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)

            head = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=8)
            role_lbl = Gtk.Label(label=role, xalign=0)
            role_lbl.add_css_class("heading")
            role_lbl.set_hexpand(True)
            link = Gtk.Button(label=name, halign=Gtk.Align.START)
            link.connect("clicked", lambda *_args,
                         target=url: self._open_link(target))
            head.append(role_lbl)
            head.append(link)

            summary_lbl = Gtk.Label(label=summary, xalign=0, wrap=True)
            summary_lbl.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
            summary_lbl.add_css_class("caption")

            row.append(head)
            row.append(summary_lbl)
            box.append(row)

        return scroll

    def _build_control(self, ctrl):
        kind = ctrl[0]
        name = ctrl[1]
        row = Gtk.ListBoxRow(activatable=False, selectable=False)
        content = Gtk.Box(
            orientation=Gtk.Orientation.VERTICAL,
            spacing=6,
            margin_start=12,
            margin_end=12,
            margin_top=10,
            margin_bottom=10,
        )
        row.set_child(content)

        title = Gtk.Label(label=t(name), xalign=0)
        title.add_css_class("heading")
        content.append(title)

        detail = desc(name)
        if detail:
            subtitle = Gtk.Label(label=detail, xalign=0, wrap=True)
            subtitle.set_wrap_mode(Pango.WrapMode.WORD_CHAR)
            subtitle.add_css_class("caption")
            content.append(subtitle)

        if kind == "toggle":
            align = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL)
            widget = Gtk.Switch(active=bool(
                read_param(name)), halign=Gtk.Align.END)
            spacer = Gtk.Box()
            spacer.set_hexpand(True)
            align.append(spacer)
            widget.connect("notify::active", self._on_switch_changed, name)
            align.append(widget)
            content.append(align)
            self._row_setters[name] = lambda v, w=widget: self._set_switch(
                w, v)
            widget.set_sensitive(WRITE_ENABLED)
            self._row_widgets[name] = widget
            return row

        if kind == "slider":
            _, _, lo, hi, step = ctrl
            value = read_param(name)
            adjustment = Gtk.Adjustment(
                value=value,
                lower=lo,
                upper=hi,
                step_increment=step,
                page_increment=max(step * 4, step),
            )
            scale_box = Gtk.Box(
                orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            scale = Gtk.Scale(
                orientation=Gtk.Orientation.HORIZONTAL, adjustment=adjustment)
            scale.set_draw_value(False)
            scale.set_hexpand(True)
            value_label = Gtk.Label(label=str(value), xalign=1)
            value_label.set_size_request(84, -1)
            scale.connect("value-changed", self._on_scale_changed,
                          name, step, value_label)
            scale_box.append(scale)
            scale_box.append(value_label)
            content.append(scale_box)
            self._row_setters[name] = (
                lambda v, s=scale, l=value_label: self._set_scale(s, l, v)
            )
            scale.set_sensitive(WRITE_ENABLED)
            self._row_widgets[name] = scale
            return row

        if kind == "entry":
            entry_box = Gtk.Box(
                orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
            entry = Gtk.Entry(
                text=str(read_param(name)),
                hexpand=True,
                input_purpose=Gtk.InputPurpose.DIGITS,
            )
            apply_btn = Gtk.Button(label=t("btn_apply"))
            apply_btn.connect(
                "clicked", lambda *_: self._commit_entry(name, entry))
            entry.connect(
                "activate", lambda *_: self._commit_entry(name, entry))
            focus = Gtk.EventControllerFocus()
            focus.connect("leave", lambda *_: self._commit_entry(name, entry))
            entry.add_controller(focus)
            entry_box.append(entry)
            entry_box.append(apply_btn)
            content.append(entry_box)
            self._row_setters[name] = lambda v, e=entry: e.set_text(str(v))
            entry.set_sensitive(WRITE_ENABLED)
            apply_btn.set_sensitive(WRITE_ENABLED)
            self._row_widgets[name] = (entry, apply_btn)
            return row

        return row

    def _write_value(self, name, value):
        if not WRITE_ENABLED:
            self.refresh_one(name)
            return False
        try:
            write_param(name, value)
            return True
        except PermissionError:
            self.show_toast(t("err_perm", name=t(name)))
        except Exception as err:
            self.show_toast(t("err_generic", name=t(name), err=err))
        self.refresh_one(name)
        return False

    def _set_switch(self, widget, value):
        self._updating = True
        widget.set_active(bool(value))
        self._updating = False

    def _set_scale(self, scale, label, value):
        self._updating = True
        scale.set_value(value)
        label.set_text(str(int(value)))
        self._updating = False

    def _on_switch_changed(self, switch, _pspec, name):
        if self._updating:
            return
        self._write_value(name, 1 if switch.get_active() else 0)

    def _on_scale_changed(self, scale, name, step, label):
        raw = scale.get_value()
        rounded = int(round(raw / step) * step)
        if self._updating:
            label.set_text(str(rounded))
            return
        if abs(raw - rounded) > 1e-6:
            self._updating = True
            scale.set_value(rounded)
            self._updating = False
        label.set_text(str(rounded))
        self._write_value(name, rounded)

    def _commit_entry(self, name, entry):
        text = entry.get_text().strip()
        try:
            value = int(text)
        except ValueError:
            self.show_toast(t("err_invalid", val=text))
            self.refresh_one(name)
            return
        self._write_value(name, value)

    def refresh_one(self, name):
        setter = self._row_setters.get(name)
        if setter is not None:
            setter(read_param(name))

    def refresh_all(self):
        for name, setter in self._row_setters.items():
            setter(read_param(name))
        mode = self._detect_mode()
        self._updating = True
        self._mode_buttons[mode].set_active(True)
        self._updating = False
        self._set_mode_hint(mode)

    def _detect_mode(self):
        return "game" if all(read_param(k) == v for k, v in GAME_PRESET.items()) else "daily"

    def _set_mode_hint(self, mode):
        if self._mode_hint is None:
            return
        key = "hint_game" if mode == "game" else "hint_daily"
        self._mode_hint.set_label(t(key))

    def _on_mode(self, mode):
        if self._updating:
            return
        preset = GAME_PRESET if mode == "game" else DAILY_PRESET
        for name, value in preset.items():
            self._write_value(name, value)
        self.refresh_all()

    def _reset_defaults(self):
        if not WRITE_ENABLED:
            return
        for name, value in DEFAULTS.items():
            self._write_value(name, value)
        self.refresh_all()
        self.show_toast(t("hint_reset_done"))

    def _request_write_mode(self):
        try:
            passthrough = [
                "DISPLAY",
                "XAUTHORITY",
                "WAYLAND_DISPLAY",
                "XDG_RUNTIME_DIR",
                "DBUS_SESSION_BUS_ADDRESS",
            ]
            env_args = [f"{k}={v}" for k in passthrough if (
                v := os.environ.get(k))]
            ready_file = Path(tempfile.gettempdir()) / \
                f"gaokun-touchscreen-tuner-ready-{uuid.uuid4().hex}"
            cmd = [
                "pkexec", "env", *env_args,
                sys.executable, os.path.abspath(
                    __file__), "--write-enabled", "--ready-file", str(ready_file),
            ]
            subprocess.Popen(cmd, start_new_session=True)
            self._wait_for_write_ready(ready_file)
        except Exception as err:
            self.show_toast(
                t("err_generic", name=t("btn_enable_edit"), err=err))

    def _wait_for_write_ready(self, ready_file):
        def poll():
            if ready_file.exists():
                self.close()
                return False
            if GLib.get_monotonic_time() - started_at > 20 * 1_000_000:
                self.show_toast(
                    t("err_generic", name=t("btn_enable_edit"), err="启动超时"))
                return False
            return True

        started_at = GLib.get_monotonic_time()
        GLib.timeout_add(200, poll)

    def _announce_write_ready(self):
        if not WRITE_ENABLED or not WRITE_READY_FILE:
            return False
        try:
            Path(WRITE_READY_FILE).write_text("ready")
        except Exception:
            return False
        return False

    def _open_link(self, url):
        try:
            open_url(url)
        except Exception as err:
            self.show_toast(t("err_open_link", err=err))


def main():
    Adw.init()
    loop = GLib.MainLoop()
    win = TunerWindow()
    win.connect("close-request", lambda *_: (loop.quit(), False)[1])
    win.present()
    if WRITE_ENABLED and WRITE_READY_FILE:
        GLib.idle_add(win._announce_write_ready)
    loop.run()
    raise SystemExit(0)


if __name__ == "__main__":
    main()
