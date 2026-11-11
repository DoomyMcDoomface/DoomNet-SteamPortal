using System;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Markup;
using System.Windows.Media;
using System.Xml;
using Microsoft.Win32;
using Forms = System.Windows.Forms;

namespace DoomNetSetup
{
    public static class Program
    {
        [STAThread]
        public static int Main(string[] args)
        {
            if (args.Length >= 2 && args[0] == "--copy-payload")
            {
                bool player = false;
                bool skin = false;
                string steam = args.Length > 2 ? args[2] : "";
                for (int i = 3; i < args.Length; i++)
                {
                    if (args[i] == "--player") player = true;
                    if (args[i] == "--skin") skin = true;
                }
                string error = Wizard.InstallFiles(args[1], steam, player, skin, AppDomain.CurrentDomain.BaseDirectory);
                return error.Length == 0 ? 0 : 1;
            }
            string folder = AppDomain.CurrentDomain.BaseDirectory;
            string xamlPath = Path.Combine(folder, "wizard.xaml");
            if (!File.Exists(xamlPath))
            {
                MessageBox.Show("Could not find wizard.xaml next to DoomNetSetup.exe.", "DoomNet Setup", MessageBoxButton.OK, MessageBoxImage.Error);
                return 1;
            }

            Window window;
            using (var reader = XmlReader.Create(xamlPath))
            {
                window = (Window)XamlReader.Load(reader);
            }

            var app = new Wizard(window);
            app.Run();
            return 0;
        }
    }

    public sealed class Wizard
    {
        readonly Window window;
        readonly TextBox steamPath;
        readonly Button backButton;
        readonly Button nextButton;
        readonly StackPanel[] pages;
        readonly Border[] marks;
        readonly TextBlock[] steps;
        readonly Border playerBubble;
        readonly TextBlock playerTick;
        readonly Border skinBubble;
        readonly TextBlock skinTick;
        readonly Border blueBubble;
        readonly TextBlock blueTick;
        readonly Border redBubble;
        readonly TextBlock redTick;
        readonly TextBlock summary;
        readonly Grid titleBar;

        int page;
        bool player;
        bool skin;
        bool matchSkin;
        bool finished;

        static readonly SolidColorBrush Red = Freeze(179, 50, 50);
        static readonly SolidColorBrush Text = Freeze(217, 218, 221);
        static readonly SolidColorBrush Muted = Freeze(143, 152, 160);
        static readonly SolidColorBrush Dim = Freeze(70, 70, 70);
        static readonly SolidColorBrush BubbleEdge = Freeze(196, 196, 196);

        public Wizard(Window window)
        {
            this.window = window;
            steamPath = Need<TextBox>("SteamPath");
            backButton = Need<Button>("BackButton");
            nextButton = Need<Button>("NextButton");
            pages = new[]
            {
                Need<StackPanel>("WelcomePage"),
                Need<StackPanel>("ChoosePage"),
                Need<StackPanel>("ColorPage"),
                Need<StackPanel>("FinishPage")
            };
            marks = new[]
            {
                Need<Border>("Mark0"),
                Need<Border>("Mark1"),
                Need<Border>("Mark2"),
                Need<Border>("Mark3")
            };
            steps = new[]
            {
                Need<TextBlock>("Step0"),
                Need<TextBlock>("Step1"),
                Need<TextBlock>("Step2"),
                Need<TextBlock>("Step3")
            };
            playerBubble = Need<Border>("PlayerBubble");
            playerTick = Need<TextBlock>("PlayerTick");
            skinBubble = Need<Border>("SkinBubble");
            skinTick = Need<TextBlock>("SkinTick");
            blueBubble = Need<Border>("BlueBubble");
            blueTick = Need<TextBlock>("BlueTick");
            redBubble = Need<Border>("RedBubble");
            redTick = Need<TextBlock>("RedTick");
            summary = Need<TextBlock>("SummaryText");
            titleBar = Need<Grid>("TitleBar");

            steamPath.Text = DetectSteam();
            Need<Button>("CloseButton").Click += (sender, args) => window.Close();
            nextButton.Click += (sender, args) => GoNext();
            backButton.Click += (sender, args) => GoBack();
            Need<Button>("BrowseButton").Click += (sender, args) => Browse();
            Need<Button>("PlayerChoice").Click += (sender, args) => TogglePlayer();
            Need<Button>("SkinChoice").Click += (sender, args) => ToggleSkin();
            Need<Button>("BlueChoice").Click += (sender, args) =>
            {
                matchSkin = false;
                Update();
            };
            Need<Button>("RedChoice").Click += (sender, args) =>
            {
                matchSkin = true;
                Update();
            };
            titleBar.MouseLeftButtonDown += Drag;
            window.PreviewKeyDown += (sender, args) =>
            {
                if (args.Key == Key.Escape)
                {
                    window.Close();
                }
            };
            Update();
        }

        public void Run()
        {
            var app = new Application();
            app.Run(window);
        }

        T Need<T>(string name) where T : class
        {
            var part = window.FindName(name) as T;
            if (part == null)
            {
                throw new InvalidOperationException("Missing " + name);
            }
            return part;
        }

        void Drag(object sender, MouseButtonEventArgs args)
        {
            DependencyObject source = args.OriginalSource as DependencyObject;
            while (source != null && source != titleBar)
            {
                if (source is Button)
                {
                    return;
                }
                source = VisualTreeHelper.GetParent(source);
            }
            try
            {
                window.DragMove();
            }
            catch (InvalidOperationException)
            {
            }
        }

        void TogglePlayer()
        {
            player = !player;
            if (!(player && skin))
            {
                matchSkin = false;
            }
            Update();
        }

        void ToggleSkin()
        {
            skin = !skin;
            if (!(player && skin))
            {
                matchSkin = false;
            }
            Update();
        }

        void GoNext()
        {
            bool both = player && skin;
            if (page == 1 && !player && !skin)
            {
                return;
            }
            if (page == 3)
            {
                if (finished)
                {
                    window.Close();
                    return;
                }
                Install();
                return;
            }
            page = page == 1 && !both ? 3 : page + 1;
            Update();
        }

        static string FindDoomNet()
        {
            string beside = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "DoomNet.exe");
            if (File.Exists(beside))
            {
                return beside;
            }
            string built = @"D:\Documents\Repos\DoomNet-SteamPortal\build\Release\DoomNet.exe";
            if (File.Exists(built))
            {
                return built;
            }
            return "";
        }

        public static string InstallFiles(string root, string steam, bool player, bool skin, string sourceDir)
        {
            string playerSource = Path.Combine(sourceDir, "payload", "player");
            string skinSource = Path.Combine(sourceDir, "payload", "skin");
            string host = Path.Combine(sourceDir, "DoomNet.exe");
            if (player && !File.Exists(Path.Combine(playerSource, "backend", "main.lua")))
            {
                return "The setup folder is missing payload\\player\\backend\\main.lua, so the player was not copied.";
            }
            if (skin && !File.Exists(Path.Combine(skinSource, "skin.json")))
            {
                return "The setup folder is missing payload\\skin\\skin.json, so the skin was not copied.";
            }
            if (!File.Exists(host))
            {
                return "DoomNet.exe is not in the setup folder, so it was not copied.";
            }
            Directory.CreateDirectory(root);
            if (player)
            {
                CopyTree(playerSource, Path.Combine(root, "player"));
            }
            if (skin)
            {
                CopyTree(skinSource, Path.Combine(root, "skin"));
            }
            File.Copy(host, Path.Combine(root, "DoomNet.exe"), true);
            string color = "steam";
            File.WriteAllText(Path.Combine(root, "config.json"),
                "{\n  \"steamPath\": " + JsonString(steam) + ",\n  \"player\": " + (player ? "true" : "false") + ",\n  \"skin\": " + (skin ? "true" : "false") + ",\n  \"playerColor\": \"" + color + "\",\n  \"streamTunesRelay\": \"\",\n  \"updates\": true\n}\n",
                new System.Text.UTF8Encoding(false));
            if (player)
            {
                string settings = Path.Combine(root, "player", "backend", "data", "settings.json");
                if (!File.Exists(settings))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(settings));
                    File.WriteAllText(settings, "{\"uiColorTheme\":\"steam\"}\n", new System.Text.UTF8Encoding(false));
                }
            }
            File.WriteAllText(Path.Combine(root, "versions.json"),
                "{\n  \"doomnet\": \"0.1.0\",\n  \"steamtunes\": " + JsonString(player ? ReadVersion(Path.Combine(playerSource, "plugin.json")) : "") + ",\n  \"steamskin\": " + JsonString(skin ? ReadVersion(Path.Combine(skinSource, "skin.json")) : "") + "\n}\n",
                new System.Text.UTF8Encoding(false));
            if (player && !File.Exists(Path.Combine(root, "player", "backend", "main.lua")))
            {
                return "The copy finished, but " + Path.Combine(root, "player", "backend", "main.lua") + " is not there.";
            }
            if (skin && !File.Exists(Path.Combine(root, "skin", "skin.json")))
            {
                return "The copy finished, but " + Path.Combine(root, "skin", "skin.json") + " is not there.";
            }
            if (!File.Exists(Path.Combine(root, "DoomNet.exe")))
            {
                return "The copy finished, but DoomNet.exe is not in " + root + ".";
            }
            return "";
        }

        static void CopyTree(string from, string to)
        {
            Directory.CreateDirectory(to);
            foreach (string file in Directory.GetFiles(from))
            {
                FileAttributes attrs = File.GetAttributes(file);
                if ((attrs & FileAttributes.ReparsePoint) != 0)
                {
                    continue;
                }
                string dest = Path.Combine(to, Path.GetFileName(file));
                if (string.Equals(Path.GetFileName(file), "settings.json", StringComparison.OrdinalIgnoreCase) && File.Exists(dest))
                {
                    continue;
                }
                File.Copy(file, dest, true);
            }
            foreach (string dir in Directory.GetDirectories(from))
            {
                FileAttributes attrs = File.GetAttributes(dir);
                if ((attrs & FileAttributes.ReparsePoint) != 0 || string.Equals(Path.GetFileName(dir), ".git", StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                CopyTree(dir, Path.Combine(to, Path.GetFileName(dir)));
            }
        }

        static string JsonString(string value)
        {
            return "\"" + (value ?? "").Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"";
        }

        static string ReadVersion(string path)
        {
            if (!File.Exists(path))
            {
                return "";
            }
            string text = File.ReadAllText(path);
            int at = text.IndexOf("\"version\"", StringComparison.Ordinal);
            if (at < 0)
            {
                return "";
            }
            int quote = text.IndexOf('"', text.IndexOf(':', at));
            int end = quote < 0 ? -1 : text.IndexOf('"', quote + 1);
            if (quote < 0 || end <= quote + 1)
            {
                return "";
            }
            return text.Substring(quote + 1, end - quote - 1);
        }

        static void OpenFolder(string root)
        {
            Process.Start(new ProcessStartInfo
            {
                FileName = "explorer.exe",
                Arguments = "\"" + root + "\"",
                UseShellExecute = true
            });
        }

        void Install()
        {
            string steam = steamPath.Text.Trim();
            if (steam.IndexOf("..", StringComparison.Ordinal) >= 0 || !File.Exists(Path.Combine(steam, "steam.exe")))
            {
                summary.Text = "Choose a Steam folder that contains steam.exe. Nothing was copied.";
                MessageBox.Show(summary.Text, "DoomNet Setup", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            string root = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DoomNet");
            string copied;
            try
            {
                copied = InstallFiles(root, steam, player, skin, AppDomain.CurrentDomain.BaseDirectory);
            }
            catch (Exception ex)
            {
                copied = "The files could not be written to " + root + ". " + ex.Message;
            }
            if (copied.Length > 0)
            {
                summary.Text = copied;
                MessageBox.Show(copied, "DoomNet Setup", MessageBoxButton.OK, MessageBoxImage.Error);
                return;
            }
            if (player && skin && matchSkin)
            {
                string settings = Path.Combine(root, "player", "backend", "data", "settings.json");
                if (File.Exists(settings) && File.ReadAllText(settings).IndexOf("\"red\"", StringComparison.Ordinal) < 0 && new FileInfo(settings).Length < 80)
                {
                    File.WriteAllText(settings, "{\"uiColorTheme\":\"red\"}\n", new System.Text.UTF8Encoding(false));
                }
                File.WriteAllText(Path.Combine(root, "config.json"),
                    "{\n  \"steamPath\": " + JsonString(steam) + ",\n  \"player\": true,\n  \"skin\": true,\n  \"playerColor\": \"red\",\n  \"streamTunesRelay\": \"\",\n  \"updates\": true\n}\n",
                    new System.Text.UTF8Encoding(false));
            }
            bool steamOpen = Process.GetProcessesByName("steam").Length > 0 || Process.GetProcessesByName("steamwebhelper").Length > 0;
            if (steamOpen)
            {
                summary.Text = "The files are in " + root + ". Quit Steam completely, including the taskbar icon, then click Install again so they can load inside Steam.";
                MessageBox.Show(summary.Text, "DoomNet Setup", MessageBoxButton.OK, MessageBoxImage.Information);
                OpenFolder(root);
                return;
            }
            string exe = FindDoomNet();
            string hookDll = Path.Combine(steam, "version.dll");
            string ourDll = Path.Combine(AppDomain.CurrentDomain.BaseDirectory, "version.dll");
            if (exe.Length > 0 && File.Exists(ourDll))
            {
                try
                {
                    Process hook = Process.Start(new ProcessStartInfo
                    {
                        FileName = exe,
                        Arguments = "--hook \"" + steam.Replace("\"", "") + "\"",
                        UseShellExecute = true,
                        Verb = "runas"
                    });
                    if (hook != null)
                    {
                        hook.WaitForExit(120000);
                    }
                }
                catch (Exception)
                {
                }
            }
            bool hookThere = File.Exists(hookDll) && File.Exists(ourDll) && new FileInfo(hookDll).Length == new FileInfo(ourDll).Length;
            OpenFolder(root);
            if (!hookThere)
            {
                summary.Text = "The player and skin are in " + root + ". version.dll is not beside steam.exe yet, so Steam will not show them until that file is copied. Click Install again and choose Yes.";
                MessageBox.Show(summary.Text, "DoomNet Setup", MessageBoxButton.OK, MessageBoxImage.Warning);
                return;
            }
            finished = true;
            nextButton.Content = "Close";
            summary.Text = "Installed into " + root + ". version.dll is beside steam.exe. Open Steam. DoomNet shows up inside Steam.";
            MessageBox.Show(summary.Text, "DoomNet Setup", MessageBoxButton.OK, MessageBoxImage.Information);
        }

        void GoBack()
        {
            if (page == 0)
            {
                return;
            }
            page = page == 3 && !(player && skin) ? 1 : page - 1;
            Update();
        }

        void Browse()
        {
            using (var dialog = new Forms.FolderBrowserDialog())
            {
                dialog.Description = "Select the folder that contains steam.exe";
                if (!string.IsNullOrEmpty(steamPath.Text) && Directory.Exists(steamPath.Text))
                {
                    dialog.SelectedPath = steamPath.Text;
                }
                if (dialog.ShowDialog() == Forms.DialogResult.OK)
                {
                    steamPath.Text = FormatWindowsPath(dialog.SelectedPath);
                }
            }
        }

        void Update()
        {
            bool both = player && skin;
            for (int i = 0; i < pages.Length; i++)
            {
                pages[i].Visibility = i == page ? Visibility.Visible : Visibility.Collapsed;
            }
            for (int i = 0; i < 4; i++)
            {
                bool skipped = i == 2 && !both && page > 2;
                if (i == page)
                {
                    marks[i].Background = Red;
                    steps[i].Foreground = Text;
                }
                else if (skipped)
                {
                    marks[i].Background = Brushes.Transparent;
                    steps[i].Foreground = Dim;
                }
                else
                {
                    marks[i].Background = Brushes.Transparent;
                    steps[i].Foreground = Muted;
                }
            }

            SetBubble(playerBubble, playerTick, player);
            SetBubble(skinBubble, skinTick, skin);
            SetBubble(blueBubble, blueTick, !matchSkin);
            SetBubble(redBubble, redTick, matchSkin);

            backButton.Visibility = page == 0 ? Visibility.Collapsed : Visibility.Visible;
            if (page == 3)
            {
                nextButton.Content = "Install";
                nextButton.IsEnabled = true;
            }
            else
            {
                nextButton.Content = "Next";
                nextButton.IsEnabled = page != 1 || player || skin;
            }

            string color = "Steam Blue";
            if (!player)
            {
                color = "not written";
            }
            else if (both && matchSkin)
            {
                color = "Match Steam-Skin";
            }
            summary.Text = "Player: " + (player ? "on" : "off")
                + "\nSkin: " + (skin ? "on" : "off")
                + "\nColor: " + color
                + "\n\nInstall copies the selected pieces into the DoomNet data folder. Steam is not started by this window.";
        }

        static void SetBubble(Border bubble, TextBlock tick, bool selected)
        {
            if (selected)
            {
                bubble.Background = Red;
                bubble.BorderBrush = Red;
                tick.Visibility = Visibility.Visible;
            }
            else
            {
                bubble.Background = Brushes.White;
                bubble.BorderBrush = BubbleEdge;
                tick.Visibility = Visibility.Collapsed;
            }
        }

        static string DetectSteam()
        {
            string path = ReadRegistry(Registry.CurrentUser, @"Software\Valve\Steam", "SteamPath");
            if (string.IsNullOrWhiteSpace(path))
            {
                path = ReadRegistry(Registry.LocalMachine, @"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath");
            }
            if (string.IsNullOrWhiteSpace(path))
            {
                string fallback = @"C:\Program Files (x86)\Steam";
                if (File.Exists(Path.Combine(fallback, "steam.exe")))
                {
                    path = fallback;
                }
            }
            return FormatWindowsPath(path);
        }

        static string ReadRegistry(RegistryKey root, string subkey, string name)
        {
            using (RegistryKey key = root.OpenSubKey(subkey))
            {
                return key == null ? "" : key.GetValue(name) as string ?? "";
            }
        }

        public static string FormatWindowsPath(string path)
        {
            if (string.IsNullOrWhiteSpace(path))
            {
                return "";
            }
            path = path.Trim().Replace('/', '\\').TrimEnd('\\');
            if (path.Length >= 2 && path[1] == ':')
            {
                path = char.ToUpperInvariant(path[0]) + path.Substring(1);
            }
            string full;
            try
            {
                full = Path.GetFullPath(path);
            }
            catch (Exception)
            {
                return path;
            }
            if (full.Length >= 2 && full[1] == ':')
            {
                full = char.ToUpperInvariant(full[0]) + full.Substring(1);
            }
            string root = Path.GetPathRoot(full);
            if (string.IsNullOrEmpty(root) || !Directory.Exists(root))
            {
                return full;
            }
            string current = root;
            string rest = full.Substring(root.Length);
            foreach (string part in rest.Split(new[] { '\\' }, StringSplitOptions.RemoveEmptyEntries))
            {
                string parent = current.TrimEnd('\\');
                if (parent.EndsWith(":", StringComparison.Ordinal))
                {
                    parent += "\\";
                }
                string found = null;
                if (Directory.Exists(parent))
                {
                    try
                    {
                        foreach (string entry in Directory.GetFileSystemEntries(parent))
                        {
                            if (string.Equals(Path.GetFileName(entry), part, StringComparison.OrdinalIgnoreCase))
                            {
                                found = entry;
                                break;
                            }
                        }
                    }
                    catch (Exception)
                    {
                        found = null;
                    }
                }
                current = found ?? Path.Combine(parent, part);
            }
            return current;
        }

        static SolidColorBrush Freeze(byte r, byte g, byte b)
        {
            var brush = new SolidColorBrush(Color.FromRgb(r, g, b));
            brush.Freeze();
            return brush;
        }
    }
}
