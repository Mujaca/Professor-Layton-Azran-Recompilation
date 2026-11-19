using System;
using System.Drawing;
using System.Runtime.InteropServices;

public class Cap
{
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    public struct RECT { public int L, T, R, B; }

    public static string Grab(IntPtr h, string path)
    {
        RECT r;
        if (!GetWindowRect(h, out r)) return "kein Rect";
        int w = r.R - r.L, ht = r.B - r.T;
        using (Bitmap bmp = new Bitmap(w, ht))
        {
            using (Graphics g = Graphics.FromImage(bmp))
            {
                IntPtr hdc = g.GetHdc();
                PrintWindow(h, hdc, 0);
                g.ReleaseHdc(hdc);
            }
            bmp.Save(path, System.Drawing.Imaging.ImageFormat.Png);
        }
        return w + "x" + ht;
    }
}
