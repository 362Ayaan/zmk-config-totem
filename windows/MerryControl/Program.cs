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
                        config.Mode = Choose("Display mode", Modes, IndexOf(Modes, config.Mode));
                        config.Save(ConfigPath); message = Repair(false);
                    }
                    else if (selected == 5)
                    {
                        int index = ChooseInt("Brightness", BrightnessValues, config.Brightness, "%");
                        config.Brightness = BrightnessValues[index]; config.Save(ConfigPath); message = Repair(false);
                    }
                    else if (selected == 6)
                    {
                        int index = ChooseInt("Screen-off timeout", TimeoutValues, config.ScreenOffSeconds, " seconds");
                        config.ScreenOffSeconds = TimeoutValues[index]; config.Save(ConfigPath); message = Repair(false);
                    }
                    else if (selected == 7) message = OpenLog();
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
                else if (key == ConsoleKey.Escape) return values[selected];
            }
        }

        private static int ChooseInt(string title, int[] values, int current, string suffix)
        {
            int selected = 0;
            for (int index = 0; index < values.Length; index++) if (values[index] == current) selected = index;
            string[] labels = new string[values.Length];
            for (int index = 0; index < values.Length; index++) labels[index] = values[index] + suffix;
            string chosen = Choose(title, labels, selected);
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
            if (response != null) return response;
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

        private static int IndexOf(string[] values, string value)
        { for (int index = 0; index < values.Length; index++) if (values[index] == value) return index; return 0; }

        private static string FormatSeconds(int seconds)
        { if (seconds >= 3600) return (seconds / 3600) + " hour"; if (seconds >= 60) return (seconds / 60) + " min"; return seconds + " sec"; }
    }
}
