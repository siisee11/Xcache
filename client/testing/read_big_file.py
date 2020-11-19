# read_big_file.py

#file_name = "hihi.txt"
#file_name = "/test/2Gdummyfile"
file_name = "/test/4Gdummyfile"
#file_name = "/test/10Gdummyfile"


for i in range(3):
    fr = open(file_name, 'r')
    fw = open(file_name + str(i) + ".copy", 'w')
    while True:
        line = fr.readline()
        if not line: break
        fw.write(line)
    fr.close()
    fw.close()
