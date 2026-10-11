import importlib.util, pathlib, unittest
spec=importlib.util.spec_from_file_location('color_error',pathlib.Path(__file__).resolve().parents[1]/'tools/color_error.py')
m=importlib.util.module_from_spec(spec);spec.loader.exec_module(m)
class ColorError(unittest.TestCase):
    def test_identical(self):self.assertEqual(m.delta_e_2000((50,2,3),(50,2,3)),0)
    def test_reference_pairs(self):
        # Sharma/Wu/Dalal published CIEDE2000 supplementary test pairs.
        for a,b,expected in [((50,2.6772,-79.7751),(50,0,-82.7485),2.0425),((50,3.1571,-77.2803),(50,0,-82.7485),2.8615),((50,0,0),(50,-1,2),2.3669)]:
            self.assertAlmostEqual(m.delta_e_2000(a,b),expected,places=4)
            self.assertAlmostEqual(m.delta_e_2000(b,a),expected,places=4)
if __name__=='__main__':unittest.main()
