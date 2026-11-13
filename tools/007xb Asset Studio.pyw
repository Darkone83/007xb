"""Double-click on Windows to launch 007xb Asset Studio (Python 3 + Tkinter)."""
from __future__ import annotations

import json
import os
import queue
import subprocess
import sys
import threading
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk
from tkinter.scrolledtext import ScrolledText

# Both the .pyw launcher and the packaged EXE are located in tools/.
# Imports are local to tools/ and work in both source and frozen mode.
if not getattr(sys, 'frozen', False):
    sys.path.insert(0, str(Path(__file__).resolve().parent))
from xb_studio_pipeline import STAGES, Pipeline, Settings, discover_inputs, STUDIO_VERSION, DEFAULT_PROJECT
ROOT = DEFAULT_PROJECT

BG = '#0E1220'
SURFACE = '#171D2C'
SURFACE2 = '#20283B'
EDGE = '#303A50'
FG = '#F0F2FC'
MUTED = '#9CA9C1'
PINK = '#E16CF4'
PURPLE = '#A18CFA'
TEAL = '#61D9CB'
RED = '#FF818D'
FONT = 'Segoe UI'
MONO = 'Consolas'


class StudioApp:
    def __init__(self, root: tk.Tk):
        self.root = root
        self.root.title(f'007xb Asset Studio • v{STUDIO_VERSION}')
        self.root.geometry('1000x820')
        self.root.minsize(860, 690)
        self.root.configure(bg=BG)
        self.root.protocol('WM_DELETE_WINDOW', self.close)
        self.events: queue.Queue = queue.Queue()
        self.submit_event = lambda kind, data: self.events.put((kind, data))
        self.stop_requested = threading.Event()
        self.worker = None
        self.running = False
        self.controls = []
        self.card_status = []
        self.card_labels = []
        self.store = ROOT / 'tools' / '.cache' / '007xb-asset-studio' / 'config.json'
        cfg = self.load_config()
        autodetected = discover_inputs(ROOT)
        self.rom = tk.StringVar(value=self.choose_default(cfg, autodetected, 'rom'))
        self.status_text = tk.StringVar(value='SELECT ROM  •  READY')
        self.summary_text = tk.StringVar(value='All converters and metadata are included. Nothing to download.')
        self.progress = tk.DoubleVar(value=0)
        self.configure_styles()
        self.create_ui()
        self.root.after(80, self.drain_events)

    @staticmethod
    def choose_default(cfg, found, key):
        previous = cfg.get(key, '')
        if previous and Path(previous).exists():
            return previous
        match = found.get(key)
        return str(match) if match else ''

    def load_config(self):
        try:
            return json.loads(self.store.read_text('utf-8'))
        except (OSError, ValueError):
            return {}

    def save_config(self):
        try:
            self.store.parent.mkdir(parents=True, exist_ok=True)
            self.store.write_text(json.dumps({
                'rom': self.rom.get(),
            }, indent=2), encoding='utf-8')
        except OSError:
            pass

    def configure_styles(self):
        style = ttk.Style()
        try: style.theme_use('clam')
        except tk.TclError: pass
        style.configure('Studio.Horizontal.TProgressbar',
                        troughcolor=SURFACE2, background=PINK, lightcolor=PINK,
                        darkcolor=PINK, bordercolor=SURFACE2, thickness=9)

    def label(self, parent, text, color=FG, size=10, weight='normal', **kwargs):
        return tk.Label(parent, text=text, font=(FONT, size, weight), fg=color,
                        bg=parent.cget('bg'), anchor='w', **kwargs)

    def btn(self, parent, title, callback, *, primary=False, width=None, lock=True):
        bg = PINK if primary else SURFACE2
        fg = '#1A1025' if primary else FG
        b = tk.Button(parent, text=title, command=callback,
                      bg=bg, fg=fg, activebackground='#EEA6F7' if primary else EDGE,
                      activeforeground=fg, highlightthickness=0, relief='flat', bd=0,
                      cursor='hand2', padx=14, pady=9, font=(FONT, 10, 'bold'),
                      width=width)
        if lock:
            self.controls.append(b)
        return b

    def create_ui(self):
        top = tk.Frame(self.root, bg=BG)
        top.pack(fill='x', padx=27, pady=(21, 9))
        brand = tk.Frame(top, bg=BG)
        brand.pack(side='left')
        self.label(brand, '007', size=26, weight='bold', color=PINK).pack(side='left')
        self.label(brand, 'xb', size=26, weight='bold', color=FG).pack(side='left')
        self.label(brand, '  /  ROM-ONLY ASSET PACKER', color=MUTED, size=12, weight='bold').pack(side='left', pady=(9, 0))
        self.label(top, f'v{STUDIO_VERSION}  •  CLASSIC RXDK PREP', color=TEAL, size=9, weight='bold').pack(side='right', pady=12)
        subtitle = tk.Frame(self.root, bg=BG)
        subtitle.pack(fill='x', padx=28, pady=(0, 13))
        self.label(subtitle, 'Select a clean GoldenEye 007 US ROM. Build one deployable Xbox archive.', MUTED, 10).pack(anchor='w')

        inputs = tk.Frame(self.root, bg=SURFACE, highlightbackground=EDGE, highlightthickness=1)
        inputs.pack(fill='x', padx=26, pady=(0, 13))
        header = tk.Frame(inputs, bg=SURFACE)
        header.pack(fill='x', padx=16, pady=(13, 5))
        self.label(header, 'YOUR ROM', color=FG, size=10, weight='bold').pack(side='left')
        self.label(header, 'All conversion tools and branding are bundled', color=MUTED, size=9).pack(side='right')
        self.make_input(inputs, 'GoldenEye US ROM', self.rom, 'rom')
        output_info = tk.Frame(inputs, bg=SURFACE)
        output_info.pack(fill='x', padx=18, pady=(9, 14))
        self.label(output_info, 'Builds game assets locally and never uploads the ROM.', color=MUTED, size=9).pack(side='left')
        self.label(output_info, 'Output: data\\007xb_assets.dat', color=TEAL, size=9).pack(side='right')

        stage_head = tk.Frame(self.root, bg=BG)
        stage_head.pack(fill='x', padx=28, pady=(1, 7))
        self.label(stage_head, 'AUTOMATIC WORKFLOW', color=FG, size=10, weight='bold').pack(side='left')
        self.label(stage_head, 'No PC-port ZIP, source checkout or additional inputs required', color=MUTED, size=9).pack(side='right')
        steps = tk.Frame(self.root, bg=BG)
        steps.pack(fill='x', padx=26)
        for i, (_, title) in enumerate(STAGES):
            card = tk.Frame(steps, bg=SURFACE, highlightbackground=EDGE, highlightthickness=1)
            card.grid(row=i//3, column=i%3, sticky='nsew', padx=3, pady=3, ipady=4)
            steps.columnconfigure(i%3, weight=1)
            heading = tk.Frame(card, bg=SURFACE)
            heading.pack(fill='x', padx=10, pady=(6, 1))
            number = self.label(heading, f'{i+1:02}', color=PURPLE, size=11, weight='bold')
            number.pack(side='left')
            status = self.label(heading, 'PENDING', color=MUTED, size=8, weight='bold')
            status.pack(side='right')
            detail = self.label(card, title, color=FG, size=10)
            detail.pack(fill='x', padx=10, pady=(0, 6))
            self.card_status.append(status)
            self.card_labels.append(detail)

        progress_box = tk.Frame(self.root, bg=BG)
        progress_box.pack(fill='x', padx=28, pady=(14, 7))
        self.label(progress_box, 'BUILD PROGRESS', size=9, weight='bold', color=MUTED).pack(anchor='w')
        ttk.Progressbar(progress_box, variable=self.progress, maximum=100,
                        mode='determinate', style='Studio.Horizontal.TProgressbar').pack(fill='x', pady=(7, 0))
        self.label(progress_box, 'Produces ONLY data/007xb_assets.dat. Everything else stays inside tools/.cache/.',
                   color=MUTED, size=9).pack(anchor='w', pady=(7,0))

        log_head = tk.Frame(self.root, bg=BG)
        log_head.pack(fill='x', padx=28, pady=(11, 5))
        self.label(log_head, 'BUILD LOG', size=10, weight='bold').pack(side='left')
        self.btn(log_head, 'Copy log', self.copy_log, lock=False).pack(side='right')
        log_box = tk.Frame(self.root, bg=EDGE)
        log_box.pack(fill='both', expand=True, padx=26, pady=(0, 12))
        self.log = ScrolledText(log_box, wrap='word', font=(MONO, 9), bg='#0B101B',
                                fg='#C8D4E6', insertbackground=FG, relief='flat', borderwidth=0,
                                padx=12, pady=9, state='disabled', height=8)
        self.log.pack(fill='both', expand=True, padx=1, pady=1)
        self.log.tag_config('good', foreground=TEAL)
        self.log.tag_config('bad', foreground=RED)
        self.log.tag_config('stage', foreground=PURPLE)
        self.log.tag_config('note', foreground=MUTED)
        self.write_log('Choose your original US GoldenEye ROM, then click Build Archive.')

        footer = tk.Frame(self.root, bg=BG)
        footer.pack(fill='x', padx=28, pady=(0, 18))
        status = tk.Frame(footer, bg=BG)
        status.pack(side='left', fill='x', expand=True)
        tk.Label(status, textvariable=self.status_text, font=(FONT, 10, 'bold'),
                 fg=TEAL, bg=BG, anchor='w').pack(fill='x')
        tk.Label(status, textvariable=self.summary_text, font=(FONT, 9),
                 fg=MUTED, bg=BG, anchor='w').pack(fill='x', pady=(3,0))
        self.btn(footer, 'Open output folder', self.open_data, lock=False).pack(side='left', padx=(0, 9))
        self.stop_button = self.btn(footer, 'Stop', self.stop)
        self.stop_button.pack(side='left', padx=(0, 9))
        self.stop_button.config(state='disabled')
        self.build_button = self.btn(footer, 'Build Archive', self.start, primary=True)
        self.build_button.pack(side='left')
        # Reserve bottom actions before the expandable log. Tk's packer otherwise
        # lets the log claim the remaining height on 768px laptop displays.
        log_box.pack_forget()
        footer.pack_forget()
        footer.pack(side='bottom', fill='x', padx=28, pady=(0, 14))
        log_box.pack(fill='both', expand=True, padx=26, pady=(0, 12))

    def make_input(self, parent, title, var, kind):
        row = tk.Frame(parent, bg=SURFACE)
        row.pack(fill='x', padx=18, pady=4)
        self.label(row, title, color=MUTED, size=9, width=19).pack(side='left')
        field = tk.Entry(row, textvariable=var, font=(FONT, 10), bg='#0F1625',
                         fg=FG, insertbackground=FG, disabledbackground='#0F1625',
                         disabledforeground=FG, relief='flat', highlightthickness=1,
                         highlightbackground=EDGE, highlightcolor=PURPLE)
        field.pack(side='left', fill='x', expand=True, ipady=6, padx=(0, 8))
        field.bind('<FocusOut>', lambda e: self.save_config())
        self.controls.append(field)
        self.btn(row, 'Browse…', lambda k=kind: self.browse(k)).pack(side='right')

    def browse(self, kind):
        spec = {
            'rom': [('N64 ROM', '*.z64 *.v64 *.n64'), ('All files', '*.*')],
        }
        p = filedialog.askopenfilename(title=f'Select {kind}', initialdir=str(ROOT), filetypes=spec[kind])
        if p:
            getattr(self,kind).set(p)
            self.save_config()

    def set_running(self, value):
        self.running = value
        for button in self.controls:
            if button in (self.build_button, self.stop_button):
                continue
            try: button.configure(state='disabled' if value else 'normal')
            except tk.TclError: pass
        self.build_button.config(state='disabled' if value else 'normal')
        self.stop_button.config(state='normal' if value else 'disabled')

    def start(self):
        if self.running:
            return
        for key, var in [('GoldenEye ROM', self.rom)]:
            if not var.get().strip():
                messagebox.showwarning('Missing input', f'Select your {key} before building.\n\nThe app could not find it automatically.')
                return
        self.save_config()
        self.stop_requested.clear()
        self.progress.set(0)
        for i, (_, name) in enumerate(STAGES):
            self.card_status[i].config(text='PENDING', fg=MUTED)
            self.card_labels[i].config(text=name, fg=FG)
        self.log.config(state='normal')
        self.log.delete('1.0', 'end')
        self.log.config(state='disabled')
        self.set_running(True)
        self.status_text.set('BUILDING ASSETS')
        self.summary_text.set('Validating ROM and building a single archive…')
        settings = Settings(
            rom=Path(self.rom.get().strip()), project=ROOT)
        self.worker = threading.Thread(target=lambda: Pipeline(settings, self.submit_event, self.stop_requested).run(),
                                       daemon=True)
        self.worker.start()

    def stop(self):
        if self.running and not self.stop_requested.is_set():
            self.stop_requested.set()
            self.status_text.set('STOP REQUESTED')
            self.summary_text.set('Stopping safely at the current operation…')
            self.stop_button.config(state='disabled')

    def write_log(self, text):
        self.log.config(state='normal')
        if text.startswith('[OK]') or text.startswith('BUILD SUCCEEDED'):
            tag = 'good'
        elif 'FAIL' in text or 'STOPPED' in text or 'ERROR' in text:
            tag = 'bad'
        elif text.startswith('[') and '/' in text and ']' in text:
            tag = 'stage'
        elif text.startswith('Note'):
            tag = 'note'
        else:
            tag = None
        self.log.insert('end', text + '\n', tag)
        # Cap UI lines; complete output stays in tools/.cache/007xb-asset-studio/build.log
        length = int(self.log.index('end-1c').split('.')[0])
        if length > 1200:
            self.log.delete('1.0', '201.0')
        self.log.see('end')
        self.log.config(state='disabled')

    def drain_events(self):
        try:
            for _ in range(120):
                kind, data = self.events.get_nowait()
                if kind == 'log':
                    self.write_log(data)
                elif kind == 'stage':
                    i = data['index']
                    self.card_status[i].config(text='RUNNING', fg=PINK)
                    self.card_labels[i].config(text=data['label'], fg=FG)
                    self.summary_text.set(data['detail'])
                elif kind == 'stage_done':
                    i = data['index']
                    self.card_status[i].config(text='DONE', fg=TEAL)
                elif kind == 'stage_skip':
                    self.card_status[data['index']].config(text='SKIPPED', fg=MUTED)
                elif kind == 'progress':
                    self.progress.set(max(0, min(100, data * 100)))
                elif kind == 'finished':
                    self.set_running(False)
                    if data['success']:
                        self.status_text.set('SINGLE ARCHIVE READY')
                        try:
                            report = json.loads((ROOT/'tools/.cache/007xb-asset-studio/asset_build_report.json').read_text('utf-8'))
                            n = report['archive']['entries']
                            mb = report['archive']['size'] / (1024 * 1024)
                            self.summary_text.set(f'007xb_assets.dat • {n} verified entries • {mb:.1f} MiB • copy only this file to Xbox')
                        except (OSError, ValueError, KeyError):
                            self.summary_text.set('Copy only data/007xb_assets.dat to Xbox D:\\data\\.')
                        self.progress.set(100)
                    else:
                        self.status_text.set('BUILD INCOMPLETE')
                        self.summary_text.set(data.get('error', 'See build.log for details')[:160])
                        for status in self.card_status:
                            if status.cget('text') == 'RUNNING':
                                status.config(text='FAILED', fg=RED)
        except queue.Empty:
            pass
        self.root.after(80, self.drain_events)

    def copy_log(self):
        content = self.log.get('1.0','end-1c')
        self.root.clipboard_clear()
        self.root.clipboard_append(content)
        self.summary_text.set('Build log copied to clipboard.')

    def open_data(self):
        folder = ROOT / 'data'
        folder.mkdir(parents=True, exist_ok=True)
        try:
            if os.name == 'nt':
                os.startfile(folder)
            elif sys.platform == 'darwin':
                subprocess.Popen(['open', str(folder)])
            else:
                subprocess.Popen(['xdg-open', str(folder)])
        except OSError as exc:
            messagebox.showinfo('Output folder', f'{folder}\n\nCould not open automatically: {exc}')

    def close(self):
        if self.running:
            messagebox.showinfo('Build active', 'Choose Stop and wait for the operation to end before closing. The build is still running.')
            return
        self.save_config()
        self.root.destroy()


def run_converter_child():
    """Run one trusted local converter in a frozen, windowless child process.

    No external network calls and no arbitrary file/script arguments allowed.
    """
    import contextlib
    import runpy
    import traceback
    allow = {'d43_emit.py', 'd69_emit.py', 'd88_emit.py'}
    log_name = os.environ.get('GE007_CONVERTER_LOG', '')
    if len(sys.argv) < 3 or sys.argv[2] not in allow or not log_name:
        return 2
    converter = (Path.cwd() / 'tools_pc' / sys.argv[2]).resolve()
    if not converter.is_file():
        return 3
    log_file = Path(log_name)
    log_file.parent.mkdir(parents=True, exist_ok=True)
    with log_file.open('a', encoding='utf-8', buffering=1) as output:
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            sys.argv = [str(converter), *sys.argv[3:]]
            sys.path.insert(0, str(converter.parent))
            try:
                runpy.run_path(str(converter), run_name='__main__')
            except SystemExit as exc:
                return exc.code if isinstance(exc.code, int) else (0 if exc.code is None else 1)
            except BaseException:
                traceback.print_exc()
                return 1
    return 0


if __name__ == '__main__':
    if len(sys.argv) > 1 and sys.argv[1] == '--007xb-converter':
        sys.exit(run_converter_child())
    # Preserve stdout-less .pyw behavior on Windows. GUI errors are shown.
    try:
        root = tk.Tk()
        # Use a short platform icon when available; never fail the app for it.
        app = StudioApp(root)
        root.mainloop()
    except Exception as exc:
        try:
            messagebox.showerror('007xb Asset Studio', f'Unable to start Studio:\n\n{exc}')
        except Exception:
            pass
        raise
