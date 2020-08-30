# read_big_file.py

f = open("5Gdummyfile", 'r')
while True:
    line = f.readline()
    if not line: break
    print(line)
f.close()
