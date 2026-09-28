using System;
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
        public static int Main()
        {
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
                window.Close();
                return;
            }
            page = page == 1 && !both ? 3 : page + 1;
            Update();
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
                nextButton.Content = "Close";
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
                + "\n\nThis is a preview. Closing the window installs nothing.";
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
