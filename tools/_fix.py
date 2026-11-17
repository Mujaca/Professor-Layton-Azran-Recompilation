import io

p = r"runtime\hle\hle.c"
s = io.open(p, encoding="utf-8").read()
s = s.replace('''        case 3: case 4: case 0x28: { /* Enable / Finalize / NotifyToWait */''',
'''        case 0x4F: { /* SetAppCpuTimeLimit */
            ipc_begin(r, 0x4F, 1, 0);
            put_result(r);
            break;
        }
        case 3: case 4: case 0x28: { /* Enable / Finalize / NotifyToWait */''')
io.open(p, "w", encoding="utf-8", newline="\n").write(s)
print("ok")
