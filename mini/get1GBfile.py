file_path = '../build/client_sent.txt'
target_size = 1024 * 1024 * 1024  # 1GB
chunk = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n" * 1024  # 每块约 37KB

with open(file_path, 'w') as f:
    written = 0
    while written < target_size:
        f.write(chunk)
        written += len(chunk.encode('utf-8'))