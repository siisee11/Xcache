# read_big_file.py

fr = open("./5Gdummyfile", 'r')
fw = open("./5Gdummyfile.copied", 'w')

while True:
    line = fr.readline()
    if not line: break
    fw.write(line)
fr.close()
fw.close()
