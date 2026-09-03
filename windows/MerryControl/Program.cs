using System;
using System.Diagnostics;
using System.IO;
using System.IO.Pipes;
using System.Runtime.Serialization;
using System.Runtime.Serialization.Json;
using System.Text;
using System.Threading;

namespace MerryDongle
{
    [DataContract]
    internal sealed class MerryConfig
    {
        [DataMember] public string Mode = "Auto";
        [DataMember] public int Brightness = 100;
        [DataMember] public int ScreenOffSeconds = 300;
        [DataMember] public string Port = "Auto";

        public static MerryConfig Load(string path)
        {
            try
            {
                using (FileStream stream = File.OpenRead(path))
                {
                    DataContractJsonSerializer serializer = new DataContractJsonSerializer(typeof(MerryConfig));
                    return serializer.ReadObject(stream) as MerryConfig ?? new MerryConfig();
                }
            }
            catch { return new MerryConfig(); }
        }

        public void Save(string path)
        {
            string temporary = path + ".tmp";
            using (FileStream stream = File.Create(temporary))
            {
                DataContractJsonSerializer serializer = new DataContractJsonSerializer(typeof(MerryConfig));
                serializer.WriteObject(stream, this);
            }
            if (File.Exists(path)) File.Replace(temporary, path, null);
            else File.Move(temporary, path);
        }
    }

    internal static class Program
    {
        private const string PipeName = "MerryDongle.Control";
        private static readonly string BaseDirectory = AppDomain.CurrentDomain.BaseDirectory;
        private static readonly string ConfigPath = Path.Combine(BaseDirectory, "MerryConfig.json");
        private static readonly string HostPath = Path.Combine(BaseDirectory, "MerryHost.exe");
        private static readonly string[] Modes = { "Auto", "Spotify", "Codex", "Pet", "Dashboard" };
        private static readonly int[] BrightnessValues = { 25, 50, 75, 100 };
        private static readonly int[] TimeoutValues = { 30, 60, 300, 600, 1800, 3600 };

        private static void Main()
        {
            Console.OutputEncoding = Encoding.UTF8;
            Console.CursorVisible = false;
            int selected = 0;
            string message = "Ready";
            while (true)
            {
                MerryConfig config = MerryConfig.Load(ConfigPath);
                string[] items = {
                    "Integration status",
                    "Start integration",
                    "Full repair / reconnect",
                    "Stop integration",
                    "Mode: " + config.Mode,
                    "Brightness: " + config.Brightness + "%",
                    "Screen-off timeout: " + FormatSeconds(config.ScreenOffSeconds),
                    "Installed pet / A-B slot",
                    "Install / change pet",
                    "Restore factory Merry",
                    "Open diagnostics log",
                    "Exit"
                };
                Draw(items, selected, message);
                ConsoleKeyInfo key = Console.ReadKey(true);
                if (key.Key == ConsoleKey.UpArrow) selected = (selected + items.Length - 1) % items.Length;
                else if (key.Key == ConsoleKey.DownArrow) selected = (selected + 1) % items.Length;
                else if (key.Key == ConsoleKey.Escape) return;
                else if (key.Key == ConsoleKey.Enter)
                {
                    if (selected == 0) message = Query("status", false);
                    else if (selected == 1) message = StartHost();
                    else if (selected == 2) message = Repair(true);
                    else if (selected == 3) message = Query("shutdown", false);
                    else if (selected == 4)
                    {
                        string chosen = Choose("Display mode", Modes, IndexOf(Modes, config.Mode));
                        if (chosen != null) { config.Mode = chosen; config.Save(ConfigPath); message = Repair(false); }
                        else message = "Display mode unchanged.";
                    }
                    else if (selected == 5)
                    {
                        int index = ChooseInt("Brightness", BrightnessValues, config.Brightness, "%");
                        if (index >= 0) { config.Brightness = BrightnessValues[index]; config.Save(ConfigPath); message = Repair(false); }
                        else message = "Brightness unchanged.";
                    }
                    else if (selected == 6)
                    {
                        int index = ChooseInt("Screen-off timeout", TimeoutValues, config.ScreenOffSeconds, " seconds");
                        if (index >= 0) { config.ScreenOffSeconds = TimeoutValues[index]; config.Save(ConfigPath); message = Repair(false); }
                        else message = "Screen-off timeout unchanged.";
                    }
                    else if (selected == 7) message = Query("petstatus", false);
                    else if (selected == 8) message = InstallPet(config);
                    else if (selected == 9) message = RestoreFactoryPet(config);
                    else if (selected == 10) message = OpenLog();
                    else return;
                }
            }
        }

        private static void Draw(string[] items, int selected, string message)
        {
            Console.Clear();
            Console.ForegroundColor = ConsoleColor.Magenta;
            Console.WriteLine("  MERRY DONGLE CONTROL");
            Console.ResetColor();
            Console.WriteLine("  One host process · PC-local settings · no scheduler\n");
            for (int index = 0; index < items.Length; index++)
            {
                if (index == selected) { Console.ForegroundColor = ConsoleColor.Black; Console.BackgroundColor = ConsoleColor.Magenta; }
                Console.WriteLine("  " + (index == selected ? "› " : "  ") + items[index].PadRight(38));
                Console.ResetColor();
            }
            Console.WriteLine("\n  ↑/↓ navigate   Enter select   Esc exit");
            Console.ForegroundColor = ConsoleColor.DarkGray;
            Console.WriteLine("\n  " + message);
            Console.ResetColor();
        }

        private static string Choose(string title, string[] values, int selected)
        {
            while (true)
            {
                Console.Clear(); Console.WriteLine("  " + title + "\n");
                for (int index = 0; index < values.Length; index++)
                {
                    if (index == selected) { Console.ForegroundColor = ConsoleColor.Black; Console.BackgroundColor = ConsoleColor.Magenta; }
                    Console.WriteLine("  " + (index == selected ? "› " : "  ") + values[index].PadRight(24));
                    Console.ResetColor();
                }
                ConsoleKey key = Console.ReadKey(true).Key;
                if (key == ConsoleKey.UpArrow) selected = (selected + values.Length - 1) % values.Length;
                else if (key == ConsoleKey.DownArrow) selected = (selected + 1) % values.Length;
                else if (key == ConsoleKey.Enter) return values[selected];
                else if (key == ConsoleKey.Escape) return null;
            }
        }

        private static int ChooseInt(string title, int[] values, int current, string suffix)
        {
            int selected = 0;
            for (int index = 0; index < values.Length; index++) if (values[index] == current) selected = index;
            string[] labels = new string[values.Length];
            for (int index = 0; index < values.Length; index++) labels[index] = values[index] + suffix;
            string chosen = Choose(title, labels, selected);
            if (chosen == null) return -1;
            for (int index = 0; index < labels.Length; index++) if (labels[index] == chosen) return index;
            return selected;
        }

        private static string StartHost()
        {
            string status = Query("status", true);
            if (status != null) return status;
            if (!File.Exists(HostPath)) return "MerryHost.exe is missing.";
            ProcessStartInfo start = new ProcessStartInfo(HostPath);
            start.UseShellExecute = false; start.CreateNoWindow = true; start.WindowStyle = ProcessWindowStyle.Hidden;
            Process.Start(start);
            for (int index = 0; index < 20; index++) { Thread.Sleep(100); status = Query("status", true); if (status != null) return status; }
            return "Host launch timed out; check diagnostics.";
        }

        private static string Repair(bool hard)
        {
            string command = hard ? "repair" : "restart";
            string response = Query(command, true);
            if (response != null)
            {
                for (int index = 0; index < 100; index++)
                {
                    Thread.Sleep(100);
                    string status = Query("status", true);
                    if (status != null && status.IndexOf("connected=True", StringComparison.OrdinalIgnoreCase) >= 0)
                        return hard ? "Full repair completed and ESP reconnected." : "Settings applied to the ESP.";
                }
                return "Restart requested; ESP reconnect is taking longer than expected.";
            }
            string started = StartHost();
            Thread.Sleep(300);
            response = Query(command, true);
            return response ?? started;
        }

        private static string Query(string command, bool quiet)
        {
            try
            {
                using (NamedPipeClientStream pipe = new NamedPipeClientStream(".", PipeName, PipeDirection.InOut))
                {
                    pipe.Connect(400);
                    using (StreamWriter writer = new StreamWriter(pipe, new UTF8Encoding(false), 1024, true))
                    using (StreamReader reader = new StreamReader(pipe, Encoding.UTF8, false, 1024, true))
                    { writer.AutoFlush = true; writer.WriteLine(command); return reader.ReadLine() ?? "No response"; }
                }
            }
            catch { return quiet ? null : "Integration host is not running."; }
        }

        private static string OpenLog()
        {
            string path = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "MerryDongle", "Logs", "MerryHost.log");
            if (!File.Exists(path)) return "No MerryHost log exists yet.";
            Process.Start(new ProcessStartInfo("notepad.exe", "\"" + path + "\"") { UseShellExecute = true });
            return "Opened diagnostics log.";
        }

        private static string InstallPet(MerryConfig config)
        {
            string installer = Path.Combine(BaseDirectory, "tools", "merry_pet_installer.ps1");
            if (!File.Exists(installer)) return "Pet installer is missing.";
            Console.Clear();
            Console.ForegroundColor = ConsoleColor.Magenta;
            Console.WriteLine("  INSTALL / CHANGE PET\n");
            Console.ResetColor();
            Console.WriteLine("  Paste a Codex Pets slug, page URL, spritesheet, or .petpack path.");
            Console.Write("\n  Pet: ");
            string source = ReadCancellableLine();
            if (source == null || source.Trim().Length == 0) return "Pet installation cancelled.";
            source = source.Trim();
            if (source.IndexOf('"') >= 0) return "Pet source cannot contain a double quote.";
            Console.WriteLine("\n  Converting and installing safely. The current pet remains valid until commit...");

            return RunPetInstaller(config, source);
        }

        private static string RestoreFactoryPet(MerryConfig config)
        {
            string factory = Path.Combine(BaseDirectory, "tools", "merry.petpack");
            if (!File.Exists(factory)) return "Factory Merry pack is missing.";
            Console.Clear();
            Console.ForegroundColor = ConsoleColor.Magenta;
            Console.WriteLine("  RESTORE FACTORY MERRY\n");
            Console.ResetColor();
            Console.WriteLine("  This installs the bundled known-good Merry pack into the inactive slot.");
            Console.WriteLine("\n  Press Enter to restore, or Esc to cancel.");
            while (true)
            {
                ConsoleKey key = Console.ReadKey(true).Key;
                if (key == ConsoleKey.Escape) return "Factory restore cancelled.";
                if (key == ConsoleKey.Enter) break;
            }
            Console.WriteLine("\n  Restoring through the verified A/B update path...");
            return RunPetInstaller(config, factory);
        }

        private static string ReadCancellableLine()
        {
            StringBuilder value = new StringBuilder();
            Console.CursorVisible = true;
            try
            {
                while (true)
                {
                    ConsoleKeyInfo key = Console.ReadKey(true);
                    if (key.Key == ConsoleKey.Escape) { Console.WriteLine(); return null; }
                    if (key.Key == ConsoleKey.Enter) { Console.WriteLine(); return value.ToString(); }
                    if (key.Key == ConsoleKey.Backspace)
                    {
                        if (value.Length > 0) { value.Length--; Console.Write("\b \b"); }
                    }
                    else if (!Char.IsControl(key.KeyChar))
                    {
                        value.Append(key.KeyChar); Console.Write(key.KeyChar);
                    }
                }
            }
            finally { Console.CursorVisible = false; }
        }

        private static string RunPetInstaller(MerryConfig config, string source)
        {
            string installer = Path.Combine(BaseDirectory, "tools", "merry_pet_installer.ps1");
            if (!File.Exists(installer)) return "Pet installer is missing.";

            Query("shutdown", true);
            Thread.Sleep(700);
            string result;
            try
            {
                ProcessStartInfo start = new ProcessStartInfo(
                    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                                 "WindowsPowerShell", "v1.0", "powershell.exe"));
                start.UseShellExecute = false;
                start.CreateNoWindow = true;
                start.WindowStyle = ProcessWindowStyle.Hidden;
                start.RedirectStandardOutput = true;
                start.RedirectStandardError = true;
                start.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
                    installer + "\" -Source \"" + source + "\"";
                if (!String.Equals(config.Port, "Auto", StringComparison.OrdinalIgnoreCase) &&
                    !String.IsNullOrWhiteSpace(config.Port))
                    start.Arguments += " -Port \"" + config.Port + "\"";
                using (Process process = Process.Start(start))
                {
                    string output = process.StandardOutput.ReadToEnd();
                    string error = process.StandardError.ReadToEnd();
                    process.WaitForExit();
                    result = process.ExitCode == 0 ? output.Trim() : error.Trim();
                    if (String.IsNullOrWhiteSpace(result))
                        result = process.ExitCode == 0 ? "Pet installed." : "Pet installation failed.";
                }
            }
            catch (Exception exception) { result = "Pet installation failed: " + exception.Message; }
            finally { Thread.Sleep(1500); StartHost(); }
            return result.Replace(Environment.NewLine, " ");
        }

        private static int IndexOf(string[] values, string value)
        { for (int index = 0; index < values.Length; index++) if (values[index] == value) return index; return 0; }

        private static string FormatSeconds(int seconds)
        { if (seconds >= 3600) return (seconds / 3600) + " hour"; if (seconds >= 60) return (seconds / 60) + " min"; return seconds + " sec"; }
    }
}
