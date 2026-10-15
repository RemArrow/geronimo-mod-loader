using System;
using System.Reflection;
using System.Windows;
using System.Windows.Threading;

namespace GMLManager
{
    public partial class App : Application
    {
        public static string Version
        {
            get
            {
                var v = Assembly.GetExecutingAssembly().GetName().Version;
                return $"{v.Major}.{v.Minor}.{v.Build}";
            }
        }

        protected override void OnStartup(StartupEventArgs e)
        {
            DispatcherUnhandledException += (s, a) =>
            {
                MessageBox.Show(a.Exception.Message, "GML Manager", MessageBoxButton.OK, MessageBoxImage.Error);
                a.Handled = true;
            };
            Core.Updater.CleanUpOldExe();
            base.OnStartup(e);
            new MainWindow().Show();
        }
    }
}
