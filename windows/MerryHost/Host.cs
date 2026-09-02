using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Pipes;
using System.Management.Automation;
using System.Management.Automation.Runspaces;
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
                    DataContractJsonSerializer serializer =
                        new DataContractJsonSerializer(typeof(MerryConfig));
                    MerryConfig value = serializer.ReadObject(stream) as MerryConfig;
                    return value ?? new MerryConfig();
                }
            }
            catch { return new MerryConfig(); }
        }
    }

    internal static class Host
    {
        private const string PipeName = "MerryDongle.Control";
        private static readonly object Gate = new object();
        private static readonly object LogGate = new object();
        private static readonly string BaseDirectory = AppDomain.CurrentDomain.BaseDirectory;
        private static readonly string ConfigPath = Path.Combine(BaseDirectory, "MerryConfig.json");
        private static readonly string ScriptPath = Path.Combine(BaseDirectory, "tools", "merry_codex_bridge.ps1");
        private static readonly string LogDirectory = Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
            "MerryDongle", "Logs");
        private static readonly string LogPath = Path.Combine(LogDirectory, "MerryHost.log");
        private static volatile bool ShutdownRequested;
        private static volatile bool RestartRequested;
        private static volatile bool BridgeRunning;
        private static volatile bool BridgeConnected;
        private static PowerShell ActivePowerShell;
        private static Thread BridgeThread;
        private static string LastError = "none";

        [STAThread]
        private static int Main()
        {
            bool created;
            using (Mutex mutex = new Mutex(true, "Local\\MerryDongleHost", out created))
            {
                if (!created) return 0;
                Directory.CreateDirectory(LogDirectory);
                RotateLog();
                Log("MerryHost starting.");
                if (!File.Exists(ScriptPath))
                {
                    Log("Bridge script missing: " + ScriptPath);
                    return 2;
                }
                StartBridgeThread();
                RunControlServer();
                StopBridge();
                if (BridgeThread != null) BridgeThread.Join(5000);
                Log("MerryHost stopped.");
                return 0;
            }
        }

        private static void StartBridgeThread()
        {
            BridgeThread = new Thread(BridgeLoop);
            BridgeThread.IsBackground = true;
            BridgeThread.Name = "Merry integration bridge";
            BridgeThread.Start();
        }

        private static void BridgeLoop()
        {
            int retrySeconds = 2;
            while (!ShutdownRequested)
            {
                MerryConfig config = MerryConfig.Load(ConfigPath);
                RestartRequested = false;
                DateTime started = DateTime.UtcNow;
                try
                {
                    InitialSessionState sessionState = InitialSessionState.CreateDefault();
                    sessionState.ExecutionPolicy = Microsoft.PowerShell.ExecutionPolicy.Bypass;
                    using (Runspace runspace = RunspaceFactory.CreateRunspace(sessionState))
                    using (PowerShell shell = PowerShell.Create())
                    {
                        runspace.Open();
                        shell.Runspace = runspace;
                        lock (Gate) { ActivePowerShell = shell; }
                        AttachStreams(shell);
                        shell.AddCommand(ScriptPath)
                             .AddParameter("Mode", config.Mode)
                             .AddParameter("Brightness", config.Brightness)
                             .AddParameter("ScreenOffSeconds", config.ScreenOffSeconds);
                        if (!String.Equals(config.Port, "Auto", StringComparison.OrdinalIgnoreCase) &&
                            !String.IsNullOrWhiteSpace(config.Port))
                            shell.AddParameter("Port", config.Port);
                        BridgeRunning = true;
                        BridgeConnected = false;
                        LastError = "none";
                        Log(String.Format("Bridge starting: mode={0}, brightness={1}, timeout={2}, port={3}",
                            config.Mode, config.Brightness, config.ScreenOffSeconds, config.Port));
                        shell.Invoke();
                        if (shell.HadErrors && !RestartRequested && !ShutdownRequested)
                            LastError = "PowerShell bridge returned an error.";
                    }
                }
                catch (Exception exception)
                {
                    if (!RestartRequested && !ShutdownRequested)
                    {
                        LastError = exception.Message;
                        Log("Bridge failure: " + exception);
                    }
                }
                finally
                {
                    BridgeRunning = false;
                    BridgeConnected = false;
                    lock (Gate) { ActivePowerShell = null; }
                }
                if (ShutdownRequested) break;
                if (RestartRequested || (DateTime.UtcNow - started).TotalMinutes >= 5)
                    retrySeconds = 2;
                Log("Bridge retry in " + retrySeconds + " seconds.");
                for (int index = 0; index < retrySeconds * 10 && !ShutdownRequested && !RestartRequested; index++)
                    Thread.Sleep(100);
                if (!RestartRequested) retrySeconds = Math.Min(retrySeconds * 2, 60);
            }
        }

        private static void AttachStreams(PowerShell shell)
        {
            shell.Streams.Error.DataAdded += delegate(object sender, DataAddedEventArgs args)
            {
                Log("ERROR " + shell.Streams.Error[args.Index]);
            };
            shell.Streams.Warning.DataAdded += delegate(object sender, DataAddedEventArgs args)
            {
                Log("WARN " + shell.Streams.Warning[args.Index]);
            };
            shell.Streams.Verbose.DataAdded += delegate(object sender, DataAddedEventArgs args)
            {
                Log("VERBOSE " + shell.Streams.Verbose[args.Index]);
            };
            shell.Streams.Information.DataAdded += delegate(object sender, DataAddedEventArgs args)
            {
                string message = shell.Streams.Information[args.Index].ToString();
                if (message.StartsWith("Merry dongle connected", StringComparison.Ordinal))
                    BridgeConnected = true;
                Log(message);
            };
        }

        private static void RunControlServer()
        {
            while (!ShutdownRequested)
            {
                using (NamedPipeServerStream pipe = new NamedPipeServerStream(
                    PipeName, PipeDirection.InOut, 1, PipeTransmissionMode.Byte,
                    PipeOptions.None))
                {
                    pipe.WaitForConnection();
                    using (StreamReader reader = new StreamReader(pipe, Encoding.UTF8, false, 1024, true))
                    using (StreamWriter writer = new StreamWriter(pipe, new UTF8Encoding(false), 1024, true))
                    {
                        writer.AutoFlush = true;
                        string command = reader.ReadLine() ?? "status";
                        writer.WriteLine(HandleCommand(command));
                    }
                }
            }
        }

        private static string HandleCommand(string command)
        {
            string normalized = command.Trim().ToLowerInvariant();
            if (normalized == "restart" || normalized == "repair")
            {
                RestartRequested = true;
                StopActiveRunspace();
                return "OK repair requested";
            }
            if (normalized == "shutdown")
            {
                ShutdownRequested = true;
                StopActiveRunspace();
                return "OK host shutting down";
            }
            MerryConfig config = MerryConfig.Load(ConfigPath);
            return String.Format("OK running={0};connected={1};mode={2};brightness={3};timeout={4};port={5};lastError={6}",
                BridgeRunning, BridgeConnected, config.Mode, config.Brightness, config.ScreenOffSeconds,
                config.Port, LastError.Replace(';', ','));
        }

        private static void StopBridge()
        {
            ShutdownRequested = true;
            StopActiveRunspace();
        }

        private static void StopActiveRunspace()
        {
            PowerShell shell;
            lock (Gate) { shell = ActivePowerShell; }
            if (shell != null)
            {
                try { shell.Stop(); } catch { }
            }
        }

        private static void RotateLog()
        {
            try
            {
                if (File.Exists(LogPath) && new FileInfo(LogPath).Length > 2 * 1024 * 1024)
                {
                    string old = LogPath + ".old";
                    if (File.Exists(old)) File.Delete(old);
                    File.Move(LogPath, old);
                }
            }
            catch { }
        }

        private static void Log(string message)
        {
            lock (LogGate)
            {
                try
                {
                    File.AppendAllText(LogPath,
                        DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss.fff") + "  " + message + Environment.NewLine,
                        Encoding.UTF8);
                }
                catch { }
            }
        }
    }
}
